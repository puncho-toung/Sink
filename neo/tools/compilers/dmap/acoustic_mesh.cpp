//puncho - SB1a (R-SB1a-30): acoustic Stage A, the mesh
#include "precompiled.h"
#pragma hdrstop

#include "acoustic.h"
#include "CompilerPhase.h"
#include "../../../idlib/MapInline.h"

/*
===============================================================================

	Stage A, the mesh (WP-SB1a)

	Only what the saved map declares stationary reaches the mesh: the world, the statics dmap inlined, and the
	statics the stationary rule passes. Every entity gets one verdict in map order. Every walk is in content order
	or dmap's own numbering (entities, map primitives, sides, surfaces, indexes), so two compiles give the same
	mesh: no order ever comes from a hash or a pointer. The mesh lives on this file's stack for one call.

===============================================================================
*/

// R-SB1a-26's reasons, in the manifest's order (in, then out), then the one SB1a-D resolves
enum acousticReason_t
{
	AR_WORLD,
	AR_INLINED,
	AR_STATIC_BRUSHES,
	AR_STATIC_COLLISION,
	AR_STATIC_TRIANGLES,
	AR_DOOR_PLUG,
	AR_DOOR_UNSEALED,
	AR_MAP_ONLY_HELPER,
	AR_NO_GEOMETRY,
	AR_NOT_STATIONARY,
	AR_KEY,
	AR_REFERENCED,
	AR_NAMED,
	AR_NAMED_IN_SCRIPT,
	AR_MODEL_FORMAT,
	AR_MODEL_MISSING,
	AR_STATIC_BUDGET_0,
	AR_STATIC_OVER_BUDGET,
	AR_NO_ACOUSTIC_SURFACE,
	AR_DOOR_START_OPEN,
	AR_DOOR_BIND,
	AR_DOOR_INHIBIT_KEY,
	AR_DOOR_MODEL_BOX_UNKNOWN,
	AR_DOOR_NO_PORTAL,
	AR_DOOR_SHARES_PORTAL,
	AR_DOOR_PENDING,		// a resolved idDoor until AcousticMesh_Doors screens it; one left fails the invariants
	AR_COUNT
};

static const char* const acousticReasonNames[AR_COUNT] =
{
	"world", "inlined", "static brushes", "static collision", "static triangles", "door plug", "door unsealed",
	"map-only helper", "no geometry", "not a stationary class", "key", "referenced", "named", "named in script",
	"model format", "model missing", "static budget 0", "static over budget", "no acoustic surface",
	"door start_open", "door bind", "door inhibit key", "door model box unknown", "door no portal", "door shares portal",
	"door pending"
};

// one per map entity, map order
struct acousticVerdict_t
{
	int						reason;		// acousticReason_t
	idStr					detail;		// empty: the manifest prints -
	int						numTris;
};

// S3a: one map value and the entity that holds it
struct acousticReference_t
{
	int						entity;
	const idStr*			value;
};

struct acousticBuild_t
{
	acousticMesh_t					mesh;
	idList<acousticVerdict_t>		verdicts;
	int								numWarnings;
	int								numKeyOut;
	idHashIndex						weld;				// mesh.verts by the bits of the vertex
	idHashIndex						materialLookup;		// mesh.materials by name, until the sort
	bool							materialOverflow;	// AcousticMesh_AddMaterial refused a 65,536th material
	idHashIndex						referenceHash;		// S3a: every map value, case-insensitive
	idList<acousticReference_t>		references;
	acousticNameSet_t				scriptTokens;		// S4: the script walk's tokens
	acousticNameSet_t				warnedMaterials;	// R-SB1a-18: one warning per material kept on a sealing face
	acousticNameSet_t				missingModels;		// CheckModel does not cache a failure (review M2-5)
};

// R-SB1a-35's transform of one entity; world: the vertex is untouched
struct acousticXform_t
{
	idMat3					axis;
	idVec3					origin;
	idVec3					pivot;
	bool					world;
	bool					mirrored;	// det( axis ) < 0: every source swaps v1, v2
};

// one source surface on its way into the mesh
struct acousticEmit_t
{
	const idMaterial*		material;
	int						index;		// mesh.materials index once resolved; -1 refused
	bool					resolved;	// added on the first triangle kept, so a surface culled whole adds no material
	bool					swap;		// v1, v2 swapped: a back-sided material, a mirroring axis
	bool					cull;		// R-SB1a-20; world hulls and plugs are not culled
	bool					dropDegenerate;	// patches, meshes, models; never hulls
	bool					full;		//puncho - SB1b-P rule 6: a static model's surface, every triangle also goes to staticTris
	int						flags;		// bit 1 two-sided
};

/*
===============================================================================

	Helpers (the names and signatures SB1a-D calls are fixed)

===============================================================================
*/

static const char* AcousticReasonName( int reason )
{
	assert( reason >= 0 && reason < AR_COUNT );
	if( reason < 0 || reason >= AR_COUNT )
	{
		return "?";
	}
	return acousticReasonNames[reason];
}

// R-SB1a-26: 2 every in reason, 0 not a candidate, 1 a candidate left out (and a door still pending, which never
// reaches a file: it fails the invariants)
static int AcousticReasonKind( int reason )
{
	if( reason >= AR_WORLD && reason <= AR_DOOR_UNSEALED )
	{
		return 2;
	}
	if( reason >= AR_MAP_ONLY_HELPER && reason <= AR_NOT_STATIONARY )
	{
		return 0;
	}
	return 1;
}

static void AcousticMesh_Warn( acousticBuild_t& build, const char* fmt, ... )
{
	char		text[1024];
	va_list		argptr;

	va_start( argptr, fmt );
	idStr::vsnPrintf( text, sizeof( text ), fmt, argptr );
	va_end( argptr );

	common->Warning( "acoustics: %s", text );
	build.numWarnings++;
}

/*
============
AcousticMesh_ResolvedKey

The map key, else the entity def's merged dict (inheritance merged, as dress.cpp reads it).
============
*/
static bool AcousticMesh_ResolvedKey( const acousticBuild_t& build, int entity, const char* key, idStr& value )
{
	const idDict& epairs = build.mesh.mapFile->GetEntity( entity )->epairs;
	const idKeyValue* kv = epairs.FindKey( key );
	if( kv == NULL )
	{
		const char* classname = epairs.GetString( "classname" );
		const idDeclEntityDef* def = ( classname[0] != '\0' ) ? static_cast<const idDeclEntityDef*>( declManager->FindType( DECL_ENTITYDEF, classname, false ) ) : NULL;
		if( def != NULL )
		{
			kv = def->dict.FindKey( key );
		}
	}
	if( kv == NULL )
	{
		value.Clear();
		return false;
	}
	value = kv->GetValue();
	return true;
}

/*
============
AcousticMesh_EntityTransform

R-SB1a-35: dmap reads the origin key and never applies it, so every primitive of an entity other than the world
is still relative to it. axis is the one the game spawns with and dmap inlines with (usurface.cpp's copy of
Entity.cpp): rotation, else angles, else angle as yaw, times modelscale_vec or modelscale. pivot is a Valve 220
origin brush's, which the game adds to the origin.
============
*/
static void AcousticMesh_EntityTransform( int entity, idMat3& axis, idVec3& origin, idVec3& pivot )
{
	axis.Identity();
	origin.Zero();
	pivot.Zero();
	if( entity == 0 )
	{
		return;
	}

	const idMapEntity* mapEnt = dmapGlobals.dmapFile->GetEntity( entity );
	const idDict& epairs = mapEnt->epairs;

	// as usurface.cpp's inline transform, line for line
	if( !epairs.GetMatrix( "rotation", "1 0 0 0 1 0 0 0 1", axis ) )
	{
		idAngles angles;
		idMat3 rotMat;
		idMat3 scaleMat;

		rotMat.Identity();
		scaleMat.Identity();

		if( epairs.GetAngles( "angles", "0 0 0", angles ) )
		{
			if( angles.pitch != 0.0f || angles.yaw != 0.0f || angles.roll != 0.0f )
			{
				rotMat = angles.ToMat3();
			}
		}
		else
		{
			float angle = epairs.GetFloat( "angle" );
			if( angle != 0.0f )
			{
				rotMat = idAngles( 0.0f, angle, 0.0f ).ToMat3();
			}
		}

		idVec3 scaleVec;
		if( epairs.GetVector( "modelscale_vec", "1 1 1", scaleVec ) )
		{
			if( ( scaleVec.x != 1.0f || scaleVec.y != 1.0f || scaleVec.z != 1.0f ) && ( scaleVec.x > 0.01f && scaleVec.y > 0.01f && scaleVec.z > 0.01f ) )
			{
				scaleMat[0][0] = scaleVec.x;
				scaleMat[1][1] = scaleVec.y;
				scaleMat[2][2] = scaleVec.z;
			}
		}
		else
		{
			float scale = epairs.GetFloat( "modelscale", 1.0f );
			if( scale != 1.0f && scale > 0.01f )
			{
				scaleMat[0][0] = scale;
				scaleMat[1][1] = scale;
				scaleMat[2][2] = scale;
			}
		}

		axis = scaleMat * rotMat;
	}

	origin = epairs.GetVector( "origin" );
	pivot = mapEnt->originOffset;
}

// w = ( p - P ) * A + O + P; a model static passes a zero pivot: v * A + O
static idVec3 AcousticMesh_ToWorld( const idVec3& p, const idMat3& axis, const idVec3& origin, const idVec3& pivot )
{
	return ( p - pivot ) * axis + origin + pivot;
}

static idVec3 AcousticMesh_Place( const acousticXform_t& xf, const idVec3& p )
{
	return xf.world ? p : AcousticMesh_ToWorld( p, xf.axis, xf.origin, xf.pivot );
}

static void AcousticMesh_SetXform( int entity, bool pivoted, acousticXform_t& xf )
{
	AcousticMesh_EntityTransform( entity, xf.axis, xf.origin, xf.pivot );
	if( !pivoted )
	{
		xf.pivot.Zero();
	}
	xf.world = ( entity == 0 );
	xf.mirrored = !xf.world && xf.axis.Determinant() < 0.0f;
}

// R-SB1a-12: the extensions dmap and the game may load as a static's model in both hosts (MapInline.h's list)
static bool AcousticMesh_ModelFormatOk( const char* model )
{
	static const char* const extensions[] = { "glb", "gltf", "lwo", "ase", "ma", "obj" };
	idStr extension;
	idStr( model ).ExtractFileExtension( extension );
	const int numExtensions = sizeof( extensions ) / sizeof( extensions[0] );
	for( int i = 0; i < numExtensions; i++ )
	{
		if( !extension.Icmp( extensions[i] ) )
		{
			return true;
		}
	}
	return false;
}

//puncho - SB1a (R-SB1a-22, review M2-1): idHashIndex keeps only a key's low bits, and a grid coordinate's float bits
// are all high bits; fold them down. For the two string keys the mix is neutral: their collisions come from idStr::Hash
static int AcousticMesh_MixKey( uint32 h )
{
	h ^= h >> 16;
	h *= 0x85ebca6bu;
	h ^= h >> 13;
	h *= 0xc2b2ae35u;
	h ^= h >> 16;
	return ( int )h;
}

/*
============
AcousticMesh_AddMaterial

NULL is the synthetic _acoustic_generic. The numbers are AcousticResolveNumbers', the game's. A material kept
on a sealing face fails the rule, so its source is taken as the rule's own order takes it (liquid contents, else
its surface type) and its preset is the table row whose numbers it resolved to.
Returns the index before the sort, or -1 past 65,535 materials.
============
*/
static int AcousticMesh_AddMaterial( acousticBuild_t& build, const idMaterial* material )
{
	const char* name = ( material != NULL ) ? material->GetName() : "_acoustic_generic";
	const int key = AcousticMesh_MixKey( ( uint32 )idStr::Hash( name ) );
	for( int i = build.materialLookup.First( key ); i != -1; i = build.materialLookup.Next( i ) )
	{
		if( !build.mesh.materials[i].name.Cmp( name ) )
		{
			return i;
		}
	}

	if( build.mesh.materials.Num() >= 65535 )
	{
		build.materialOverflow = true;
		return -1;
	}

	acousticMaterial_t entry;
	entry.name = name;
	if( material == NULL )
	{
		entry.source = ACOUSTIC_SOURCE_SYNTHETIC;
		entry.preset = ACOUSTIC_PRESET_GENERIC;
		AcousticPresetNumbers( ACOUSTIC_PRESET_GENERIC, entry.n );
	}
	else
	{
		AcousticResolveNumbers( material, entry.n );
		int source, preset;
		if( AcousticMaterialPasses( material, source, preset ) )
		{
			entry.source = source;
			entry.preset = preset;
		}
		else
		{
			entry.source = ( material->GetContentFlags() & ( CONTENTS_WATER | CONTENTS_LAVA | CONTENTS_SLIME ) ) ? ACOUSTIC_SOURCE_LIQUID : material->GetSurfaceType();
			entry.preset = ACOUSTIC_PRESET_GENERIC;
			for( int p = 0; p < ACOUSTIC_PRESET_COUNT; p++ )
			{
				acousticNumbers_t row;
				AcousticPresetNumbers( p, row );
				if( !memcmp( &row, &entry.n, sizeof( row ) ) )
				{
					entry.preset = p;
					break;
				}
			}
		}
	}

	const int index = build.mesh.materials.Append( entry );
	build.materialLookup.Add( key, index );
	return index;
}

// the bits of the three floats: exact-bit welding (R-SB1a-22)
static int AcousticMesh_VertexKey( const idVec3& v )
{
	uint32 bits[3];
	memcpy( bits, v.ToFloatPtr(), sizeof( bits ) );
	// mixed per coordinate, so a coarse grid does not fold distinct points onto few keys (review M2R-4)
	uint32 h = ( uint32 )AcousticMesh_MixKey( bits[0] );
	h = ( uint32 )AcousticMesh_MixKey( h ^ bits[1] );
	return AcousticMesh_MixKey( h ^ bits[2] );
}

// a list about to grow doubles its capacity, so growth copies O(N) elements, not N^2 / granularity (review M2-3);
//puncho - SB1b-P (review P4R-6): acoustic.h's AcousticReserveOne, shared with the probes, minimum 16,384 as before
static const int ACOUSTIC_MESH_MIN_CAPACITY = 16384;

static int AcousticMesh_Weld( acousticBuild_t& build, const idVec3& v )
{
	// review M2-7: idWinding::Split writes -0 on a zero-distance plane; one sign, so a coordinate plane welds. Moves
	// nothing. On the bits, which no floating-point model can change (review M2R-3)
	uint32 bits[3];
	memcpy( bits, v.ToFloatPtr(), sizeof( bits ) );
	for( int axis = 0; axis < 3; axis++ )
	{
		if( bits[axis] == 0x80000000u )
		{
			bits[axis] = 0;
		}
	}
	idVec3 w;
	memcpy( w.ToFloatPtr(), bits, sizeof( bits ) );

	const int key = AcousticMesh_VertexKey( w );
	for( int i = build.weld.First( key ); i != -1; i = build.weld.Next( i ) )
	{
		if( !memcmp( &build.mesh.verts[i], &w, sizeof( idVec3 ) ) )
		{
			return i;
		}
	}

	AcousticReserveOne( build.mesh.verts, ACOUSTIC_MESH_MIN_CAPACITY );
	const int index = build.mesh.verts.Append( w );

	// review M2-3: past four vertices a bucket, a table four times the size, every vertex re-added in index order. The
	// table's order never reaches the output and every hit is confirmed by memcmp, so the mesh is unchanged
	const int hashSize = build.weld.GetHashSize();
	if( build.mesh.verts.Num() > 4 * hashSize )
	{
		build.weld.Clear( 4 * hashSize, 2 * build.mesh.verts.Num() );
		for( int i = 0; i < build.mesh.verts.Num(); i++ )
		{
			build.weld.Add( AcousticMesh_VertexKey( build.mesh.verts[i] ), i );
		}
		return index;
	}
	if( index >= build.weld.GetIndexSize() )
	{
		build.weld.ResizeIndex( 2 * build.weld.GetIndexSize() );
	}
	build.weld.Add( key, index );
	return index;
}

/*
============
AcousticMesh_AddTri

Welds by exact bits, vertices indexed in order of first use. Never culls, flips or drops: the callers do.
============
*/
static void AcousticMesh_AddTri( acousticBuild_t& build, const idVec3& v0, const idVec3& v1, const idVec3& v2, int material, int flags )
{
	assert( material >= 0 && material < 65535 );
	AcousticReserveOne( build.mesh.tris, ACOUSTIC_MESH_MIN_CAPACITY );
	acousticTri_t& tri = build.mesh.tris.Alloc();
	tri.v[0] = AcousticMesh_Weld( build, v0 );
	tri.v[1] = AcousticMesh_Weld( build, v1 );
	tri.v[2] = AcousticMesh_Weld( build, v2 );
	tri.material = ( uint16 )material;
	tri.flags = ( uint16 )flags;
}

// the winding WriteOutputPortals writes for interAreaPortals[portal]
static const idWinding* AcousticMesh_PortalWinding( int portal )
{
	const interAreaPortal_t& iap = interAreaPortals[portal];
	if( iap.side != NULL )
	{
		return iap.side->winding;
	}
	return &iap.w;
}

/*
============
AcousticMesh_TouchesOpenLeaf_r

R-SB1a-20 (O-SB1a-1): the triangle is kept whole if it touches a world leaf with an area and not opaque. Descent
without splitting, classified as idWinding::Split classifies; a coplanar triangle goes front when its normal agrees
with the plane's. Stops at the first open leaf.
============
*/
static bool AcousticMesh_TouchesOpenLeaf_r( const node_t* node, const idVec3 v[3], const idVec3& normal )
{
	while( node->planenum != PLANENUM_LEAF )
	{
		const idPlane& plane = dmapGlobals.mapPlanes[node->planenum];
		int front = 0;
		int back = 0;
		for( int k = 0; k < 3; k++ )
		{
			const float d = plane.Distance( v[k] );
			if( d > ON_EPSILON )
			{
				front++;
			}
			else if( d < -ON_EPSILON )
			{
				back++;
			}
		}

		if( !front && !back )
		{
			node = ( normal * plane.Normal() > 0.0f ) ? node->children[0] : node->children[1];
		}
		else if( !front )
		{
			node = node->children[1];
		}
		else if( !back )
		{
			node = node->children[0];
		}
		else
		{
			if( AcousticMesh_TouchesOpenLeaf_r( node->children[0], v, normal ) )
			{
				return true;
			}
			node = node->children[1];
		}
	}
	return !node->opaque && node->area >= 0;
}

static void AcousticMesh_InitEmit( acousticEmit_t& emit, const idMaterial* material )
{
	emit.material = material;
	emit.index = -1;
	emit.resolved = false;
	emit.swap = false;
	emit.cull = true;
	emit.dropDegenerate = true;
	emit.full = false;
	emit.flags = 0;
}

// R-SB1a-33: a back-sided material's drawn side is the back; a two-sided one has no air side (flag bit 1). facing is
// the surface's own material, even where the acoustic record is borrowed (a collision surface, review M2-2)
static void AcousticMesh_SetSided( acousticEmit_t& emit, const idMaterial* facing, const acousticXform_t& xf )
{
	bool back = false;
	if( facing != NULL )
	{
		const cullType_t cullType = facing->GetCullType();
		if( cullType == CT_TWO_SIDED )
		{
			emit.flags |= 2;
		}
		else if( cullType == CT_BACK_SIDED )
		{
			back = true;
		}
	}
	emit.swap = ( back != xf.mirrored );
}

/*
============
AcousticMesh_Emit

One triangle in world space, in its source's order: front on ( v2 - v0 ) x ( v1 - v0 ) after the swap.
============
*/
static void AcousticMesh_Emit( acousticBuild_t& build, acousticEmit_t& emit, const idVec3& a, const idVec3& b, const idVec3& c )
{
	const idVec3 v[3] = { a, emit.swap ? c : b, emit.swap ? b : c };
	const idVec3 normal = ( v[2] - v[0] ).Cross( v[1] - v[0] );

	// as dmap drops its own (usurface.cpp:727); nothing is snapped
	if( emit.dropDegenerate && normal.x == 0.0f && normal.y == 0.0f && normal.z == 0.0f )
	{
		return;
	}
	// the cull's one decision (R-SB1a-20), made once, as before
	const bool kept = !emit.cull || AcousticMesh_TouchesOpenLeaf_r( build.mesh.worldTree->headnode, v, normal );

	//puncho - SB1b-P (rule 6): the same points in the same winding as the kept triangle, and whether the cull removed
	// it; touches no weld, material or mesh.tris, so the mesh and every output are as without it
	if( emit.full && ( normal.x != 0.0f || normal.y != 0.0f || normal.z != 0.0f ) )
	{
		AcousticReserveOne( build.mesh.staticTris, ACOUSTIC_MESH_MIN_CAPACITY );
		acousticStaticTri_t& full = build.mesh.staticTris.Alloc();
		full.v[0] = v[0];
		full.v[1] = v[1];
		full.v[2] = v[2];
		full.flags = emit.flags | ( kept ? 0 : ACOUSTIC_STATIC_TRI_CULLED );
	}
	if( !kept )
	{
		return;
	}
	if( !emit.resolved )
	{
		emit.index = AcousticMesh_AddMaterial( build, emit.material );
		emit.resolved = true;
	}
	if( emit.index < 0 )
	{
		return;		// past 65,535 materials: Stage A fails after the geometry
	}
	AcousticMesh_AddTri( build, v[0], v[1], v[2], emit.index, emit.flags );
}

static const idMaterial* AcousticMesh_Skinned( const idMaterial* material, const idDeclSkin* skin )
{
	return ( skin != NULL && material != NULL ) ? skin->RemapShaderBySkin( material ) : material;
}

// a static's skin (draw-only, R-SB1a-10); NULL: none
static const idDeclSkin* AcousticMesh_EntitySkin( const idDict& epairs )
{
	const char* skinName = epairs.GetString( "skin" );
	return ( skinName[0] != '\0' ) ? declManager->FindSkin( skinName, false ) : NULL;
}

static bool AcousticMesh_Passes( const idMaterial* material )
{
	int source, preset;
	return AcousticMaterialPasses( material, source, preset );
}

// R-SB1a-18: every face that seals the BSP is in the mesh, whatever the rule says
static void AcousticMesh_WarnSealing( acousticBuild_t& build, const idMaterial* material )
{
	const char* name = ( material != NULL ) ? material->GetName() : "_acoustic_generic";
	if( AcousticNameSet_Contains( build.warnedMaterials, name ) )
	{
		return;
	}
	AcousticNameSet_Add( build.warnedMaterials, name, idStr::Length( name ) );
	AcousticMesh_Warn( build, "material '%s' kept on a sealing face", name );
}

/*
============
AcousticMesh_EmitBrush

R-SB1a-18: each side's visibleHull, fanned ( 0, k, k + 1 ) as the .proc fans it; its front is out of the brush.
Hulls ignore cull type and are never dropped; a world hull is never culled.
============
*/
static void AcousticMesh_EmitBrush( acousticBuild_t& build, const uBrush_t* brush, const acousticXform_t& xf, const idDeclSkin* skin )
{
	for( int s = 0; s < brush->numsides; s++ )
	{
		const side_t* side = &brush->sides[s];
		const idWinding* w = side->visibleHull;
		if( w == NULL || w->GetNumPoints() < 3 )
		{
			continue;
		}

		const idMaterial* material = AcousticMesh_Skinned( side->material, skin );
		if( !AcousticMesh_Passes( material ) )
		{
			if( !xf.world || !brush->opaque )
			{
				continue;
			}
			AcousticMesh_WarnSealing( build, material );
		}

		acousticEmit_t emit;
		AcousticMesh_InitEmit( emit, material );
		emit.swap = xf.mirrored;
		emit.cull = !xf.world;
		emit.dropDegenerate = false;

		const idVec3 p0 = AcousticMesh_Place( xf, ( *w )[0].ToVec3() );
		idVec3 previous = AcousticMesh_Place( xf, ( *w )[1].ToVec3() );
		for( int k = 1; k + 1 < w->GetNumPoints(); k++ )
		{
			const idVec3 next = AcousticMesh_Place( xf, ( *w )[k + 1].ToVec3() );
			AcousticMesh_Emit( build, emit, p0, previous, next );
			previous = next;
		}
	}
}

/*
============
AcousticMesh_EmitPatch

R-SB1a-19: tessellated as the collision model tessellates it, triangles ( i + 1, i + 2, i ) as map.cpp takes them.
============
*/
static void AcousticMesh_EmitPatch( acousticBuild_t& build, const idMapPatch* patch, const acousticXform_t& xf, const idDeclSkin* skin )
{
	const idMaterial* material = AcousticMesh_Skinned( declManager->FindMaterial( patch->GetMaterial() ), skin );
	if( !AcousticMesh_Passes( material ) )
	{
		return;
	}

	idSurface_Patch cp( *patch );
	if( patch->GetExplicitlySubdivided() )
	{
		cp.SubdivideExplicit( patch->GetHorzSubdivisions(), patch->GetVertSubdivisions(), false, true );
	}
	else
	{
		cp.Subdivide( DEFAULT_CURVE_MAX_ERROR_CD, DEFAULT_CURVE_MAX_ERROR_CD, DEFAULT_CURVE_MAX_LENGTH_CD, false );
	}

	acousticEmit_t emit;
	AcousticMesh_InitEmit( emit, material );
	AcousticMesh_SetSided( emit, material, xf );
	emit.full = true;	//puncho - SB1b-P rule 6 (review P4R-3): a patch can close a solid rule 5 does not see

	const int* indexes = cp.GetIndexes();
	for( int i = 0; i + 2 < cp.GetNumIndexes(); i += 3 )
	{
		AcousticMesh_Emit( build, emit,
						   AcousticMesh_Place( xf, cp[indexes[i + 1]].xyz ),
						   AcousticMesh_Place( xf, cp[indexes[i + 2]].xyz ),
						   AcousticMesh_Place( xf, cp[indexes[i + 0]].xyz ) );
	}
}

/*
============
AcousticMesh_EmitPolygonMesh

R-SB1a-19: from the MapPolygonMesh, fan ( 0, j, j + 1 ), the order PutPrimitivesInAreas restores. Every triangle
of an opaque world mesh seals the BSP and is kept whatever the rule says (R-SB1a-18).
============
*/
static void AcousticMesh_EmitPolygonMesh( acousticBuild_t& build, const MapPolygonMesh* mesh, const acousticXform_t& xf, const idDeclSkin* skin )
{
	const idList<idDrawVert>& verts = mesh->GetDrawVerts();
	const bool sealing = xf.world && mesh->IsOpaque();

	for( int f = 0; f < mesh->GetNumPolygons(); f++ )
	{
		const MapPolygon& poly = mesh->GetFace( f );
		const idList<int>& indexes = poly.GetIndexes();
		if( indexes.Num() < 3 )
		{
			continue;
		}

		const idMaterial* material = AcousticMesh_Skinned( declManager->FindMaterial( poly.GetMaterial() ), skin );
		if( !AcousticMesh_Passes( material ) )
		{
			if( !sealing )
			{
				continue;
			}
			AcousticMesh_WarnSealing( build, material );
		}

		acousticEmit_t emit;
		AcousticMesh_InitEmit( emit, material );
		AcousticMesh_SetSided( emit, material, xf );
		// review M2-4, M2R-1: a face that seals the BSP is never culled, and the BSP, not the cull type, gives its air
		// side, as a world hull's (the world is never mirrored; xf.mirrored is kept for the rule's form)
		emit.cull = !sealing;
		if( sealing )
		{
			emit.swap = xf.mirrored;
		}
		//puncho - SB1b-P rule 6 (review P4R-3, P4S-2): a polygon mesh can close a solid rule 5 does not see; a sealing
		// face is never stored: it is never culled, and the BSP already makes what lies behind it solid
		emit.full = !sealing;

		const idVec3 p0 = AcousticMesh_Place( xf, verts[indexes[0]].xyz );
		for( int j = 1; j + 1 < indexes.Num(); j++ )
		{
			AcousticMesh_Emit( build, emit, p0, AcousticMesh_Place( xf, verts[indexes[j]].xyz ), AcousticMesh_Place( xf, verts[indexes[j + 1]].xyz ) );
		}
	}
}

static bool AcousticMesh_IsCollisionSurface( const modelSurface_t* surface )
{
	return surface->shader != NULL && ( surface->shader->GetSurfaceFlags() & SURF_COLLISION ) != 0;
}

static bool AcousticMesh_ModelHasCollision( const idRenderModel* model )
{
	for( int i = 0; i < model->NumSurfaces(); i++ )
	{
		if( AcousticMesh_IsCollisionSurface( model->Surface( i ) ) )
		{
			return true;
		}
	}
	return false;
}

// R-SB1a-12: the material of the drawn surface with most triangles (ties: lowest index); NULL is _acoustic_generic
static const idMaterial* AcousticMesh_DrawnMaterial( const idRenderModel* model, const idDeclSkin* skin )
{
	const idMaterial* best = NULL;
	int bestTris = 0;
	for( int i = 0; i < model->NumSurfaces(); i++ )
	{
		const modelSurface_t* surface = model->Surface( i );
		if( surface->geometry == NULL || AcousticMesh_IsCollisionSurface( surface ) )
		{
			continue;
		}
		const idMaterial* material = AcousticMesh_Skinned( surface->shader, skin );
		const int numTris = surface->geometry->numIndexes / 3;
		if( material != NULL && material->IsDrawn() && numTris > bestTris )
		{
			best = material;
			bestTris = numTris;
		}
	}
	return best;
}

// R-SB1a-12: the triangles of the surfaces that pass the rule, the count the budget reads
static int AcousticMesh_CountModelTris( const idRenderModel* model, const idDeclSkin* skin )
{
	int count = 0;
	for( int i = 0; i < model->NumSurfaces(); i++ )
	{
		const modelSurface_t* surface = model->Surface( i );
		if( surface->geometry != NULL && AcousticMesh_Passes( AcousticMesh_Skinned( surface->shader, skin ) ) )
		{
			count += surface->geometry->numIndexes / 3;
		}
	}
	return count;
}

/*
============
AcousticMesh_EmitModel

R-SB1a-9, R-SB1a-12: surfaces and indexes in order, v * A + O. collision: the SURF_COLLISION surfaces only, each
with its own material if that declares a surface type, else the drawn one's. inlineAll: the surfaces dmap's inline
skips are skipped here too (usurface.cpp:940-947).
============
*/
static void AcousticMesh_EmitModel( acousticBuild_t& build, const idRenderModel* model, const acousticXform_t& xf, const idDeclSkin* skin, bool collision, bool inlineAll )
{
	const idMaterial* drawn = collision ? AcousticMesh_DrawnMaterial( model, skin ) : NULL;

	for( int i = 0; i < model->NumSurfaces(); i++ )
	{
		const modelSurface_t* surface = model->Surface( i );
		const srfTriangles_t* geometry = surface->geometry;
		if( geometry == NULL )
		{
			continue;
		}

		const idMaterial* material;
		if( collision )
		{
			if( !AcousticMesh_IsCollisionSurface( surface ) )
			{
				continue;
			}
			material = ( surface->shader->GetSurfaceType() != SURFTYPE_NONE ) ? surface->shader : drawn;
		}
		else
		{
			if( inlineAll && ( surface->shader == NULL || !surface->shader->IsValid() || !idStr::Cmp( surface->shader->GetName(), "_default" ) ) )
			{
				continue;
			}
			material = AcousticMesh_Skinned( surface->shader, skin );
			if( !AcousticMesh_Passes( material ) )
			{
				continue;
			}
		}

		acousticEmit_t emit;
		AcousticMesh_InitEmit( emit, material );
		emit.full = true;	//puncho - SB1b-P rule 6: only model statics reach this function (AcousticMesh_EmitEntity)
		// a collision surface borrows only the record and numbers; its facing is its own material's (review M2-2)
		AcousticMesh_SetSided( emit, collision ? surface->shader : material, xf );

		for( int j = 0; j + 2 < geometry->numIndexes; j += 3 )
		{
			AcousticMesh_Emit( build, emit,
							   AcousticMesh_Place( xf, geometry->verts[geometry->indexes[j + 0]].xyz ),
							   AcousticMesh_Place( xf, geometry->verts[geometry->indexes[j + 1]].xyz ),
							   AcousticMesh_Place( xf, geometry->verts[geometry->indexes[j + 2]].xyz ) );
		}
	}
}

// R-SB1a-23: an entity's brushes by map primitive index, gathered once (uEntity_t::primitives is reversed)
static void AcousticMesh_GatherBrushes( int entity, idList<const uBrush_t*>& byPrimitive )
{
	const uEntity_t& uEnt = dmapGlobals.uEntities[entity];
	const int numPrimitives = uEnt.mapEntity->GetNumPrimitives();
	byPrimitive.SetNum( numPrimitives );
	for( int p = 0; p < numPrimitives; p++ )
	{
		byPrimitive[p] = NULL;
	}
	for( const primitive_t* prim = uEnt.primitives; prim != NULL; prim = prim->next )
	{
		if( prim->brush != NULL && prim->brush->brushnum >= 0 && prim->brush->brushnum < numPrimitives )
		{
			byPrimitive[prim->brush->brushnum] = prim->brush;
		}
	}
}

/*
============
AcousticMesh_EmitBrushPlanes

R-SB1a-38: one record per brush of a `static brushes` entity, one world-space plane per side with a winding,
rebuilt from three of its points through the entity's transform, so a modelscale_vec static stays right.
Review M2-6: only a brush that put a triangle into the mesh gets a record (an origin, clip or nodraw brush has no
acoustic surface and must not reject probes), and a record with fewer than 4 planes bounds no volume and is dropped.
============
*/
static void AcousticMesh_EmitBrushPlanes( acousticBuild_t& build, int entity, const idList<const uBrush_t*>& byPrimitive, const idList<int>& brushTris, const acousticXform_t& xf )
{
	for( int p = 0; p < byPrimitive.Num(); p++ )
	{
		const uBrush_t* brush = byPrimitive[p];
		if( brush == NULL || brushTris[p] == 0 )
		{
			continue;
		}

		acousticBrush_t record;
		record.entity = entity;
		record.firstPlane = build.mesh.brushPlanes.Num();
		record.numPlanes = 0;

		for( int s = 0; s < brush->numsides; s++ )
		{
			const idWinding* w = brush->sides[s].winding;
			if( w == NULL || w->GetNumPoints() < 3 )
			{
				continue;
			}
			const idVec3 w0 = ( *w )[0].ToVec3();
			for( int k = 1; k + 1 < w->GetNumPoints(); k++ )
			{
				const idVec3 wk = ( *w )[k].ToVec3();
				const idVec3 wk1 = ( *w )[k + 1].ToVec3();
				const idVec3 cross = ( w0 - wk ).Cross( wk1 - wk );
				if( cross.x == 0.0f && cross.y == 0.0f && cross.z == 0.0f )
				{
					continue;
				}
				idPlane plane;
				if( plane.FromPoints( AcousticMesh_Place( xf, w0 ), AcousticMesh_Place( xf, wk ), AcousticMesh_Place( xf, wk1 ) ) )
				{
					build.mesh.brushPlanes.Append( xf.mirrored ? -plane : plane );
					record.numPlanes++;
				}
				break;
			}
		}

		if( record.numPlanes < 4 )
		{
			build.mesh.brushPlanes.SetNum( record.firstPlane );
			continue;
		}
		build.mesh.brushes.Append( record );
	}
}

/*
============
AcousticMesh_Decide

R-SB1a-8 to R-SB1a-12: one entity's verdict. Geometry is emitted later, in (c).
============
*/
static void AcousticMesh_Decide( acousticBuild_t& build, int entity, acousticVerdict_t& verdict )
{
	const idMapFile* mapFile = build.mesh.mapFile;
	const idDict& world = mapFile->GetEntity( 0 )->epairs;
	const idMapEntity* mapEnt = mapFile->GetEntity( entity );
	const idDict& epairs = mapEnt->epairs;

	verdict.detail.Clear();
	verdict.numTris = 0;

	if( entity == 0 )
	{
		verdict.reason = AR_WORLD;
		return;
	}
	if( dmapGlobals.uEntities[entity].dmapOnly )
	{
		verdict.reason = AR_MAP_ONLY_HELPER;
		return;
	}

	const char* classname = epairs.GetString( "classname" );
	const idStr name = epairs.GetString( "name" );
	const idStr modelName = epairs.GetString( "model" );

	if( !idStr::Icmp( classname, "func_static" ) || !idStr::Icmp( classname, "misc_model" ) )
	{
		// R-SB1a-9: the game hides it, dmap drew its model into the world: in, whatever its keys
		const char* inlineReason;
		if( MapInlineEligible( world, epairs, inlineReason ) && modelName.Icmp( name ) != 0 )
		{
			const idRenderModel* model = renderModelManager->FindModel( modelName );
			verdict.reason = ( model == NULL || model->IsDefaultModel() ) ? AR_MODEL_MISSING : AR_INLINED;
			return;
		}

		// S2 keys
		static const char* const allowed[] =
		{
			"classname", "name", "origin", "angle", "angles", "rotation", "model", "inline", "solid", "noclipmodel", "modelscale", "modelscale_vec",
			// draw-only: read only into the render entity
			"skin", "_color", "shaderParm3", "shaderParm4", "shaderParm5", "shaderParm6", "shaderParm7", "shaderParm8", "shaderParm9",
			"shaderParm10", "shaderParm11", "noshadows", "noselfshadows", "noDynamicInteractions", "maxVisDist", "minVisPixels",
			"cullShadow", "lodScale"
		};
		const int numAllowed = sizeof( allowed ) / sizeof( allowed[0] );
		for( int k = 0; k < epairs.GetNumKeyVals(); k++ )
		{
			const idStr& key = epairs.GetKeyVal( k )->GetKey();
			bool ok = ( key.Icmpn( "_tb_", 4 ) == 0 );
			for( int i = 0; !ok && i < numAllowed; i++ )
			{
				ok = ( key.Icmp( allowed[i] ) == 0 );
			}
			if( !ok )
			{
				verdict.reason = AR_KEY;
				verdict.detail = key;
				build.numKeyOut++;
				return;
			}
		}

		if( !name.IsEmpty() )
		{
			// S3a: the lowest other entity holding the name as a value
			int lowest = -1;
			const int key = AcousticMesh_MixKey( ( uint32 )idStr::IHash( name.c_str() ) );
			for( int r = build.referenceHash.First( key ); r != -1; r = build.referenceHash.Next( r ) )
			{
				const acousticReference_t& ref = build.references[r];
				if( ref.entity != entity && !ref.value->Icmp( name ) && ( lowest == -1 || ref.entity < lowest ) )
				{
					lowest = ref.entity;
				}
			}
			if( lowest != -1 )
			{
				verdict.reason = AR_REFERENCED;
				verdict.detail.Format( "%i", lowest );
				return;
			}

			// S3b: classname, an optional _, digits (Game_local.cpp's test)
			const int prefix = idStr::Length( classname );
			const char* suffix = name.c_str() + Min( prefix, name.Length() );
			if( *suffix == '_' )
			{
				suffix++;
			}
			bool generated = name.Icmpn( classname, prefix ) == 0 && *suffix != '\0';
			for( ; *suffix != '\0'; suffix++ )
			{
				generated &= *suffix >= '0' && *suffix <= '9';
			}
			if( !generated )
			{
				verdict.reason = AR_NAMED;
				return;
			}

			// S4: $name or "name" a token of the script walk
			if( AcousticNameSet_Contains( build.scriptTokens, name.c_str() ) )
			{
				verdict.reason = AR_NAMED_IN_SCRIPT;
				return;
			}
		}

		// R-SB1a-11
		if( mapEnt->GetNumPrimitives() > 0 )
		{
			verdict.reason = AR_STATIC_BRUSHES;
			return;
		}
		if( modelName.IsEmpty() )
		{
			verdict.reason = AR_NO_GEOMETRY;
			return;
		}

		// R-SB1a-12: model geometry, equal in both hosts
		if( !AcousticMesh_ModelFormatOk( modelName ) )
		{
			verdict.reason = AR_MODEL_FORMAT;
			modelName.ExtractFileExtension( verdict.detail );
			return;
		}
		// budget 0 is C-SB22's switch: no model static is parsed, collision ones included (director's ruling)
		const int budget = build.mesh.settings.staticTriBudget;
		if( budget == 0 )
		{
			verdict.reason = AR_STATIC_BUDGET_0;
			return;
		}
		if( AcousticNameSet_Contains( build.missingModels, modelName.c_str() ) )
		{
			verdict.reason = AR_MODEL_MISSING;
			return;
		}
		const idRenderModel* model = renderModelManager->CheckModel( modelName );
		if( model == NULL || model->IsDefaultModel() )
		{
			AcousticNameSet_Add( build.missingModels, modelName.c_str(), modelName.Length() );
			verdict.reason = AR_MODEL_MISSING;
			return;
		}
		if( AcousticMesh_ModelHasCollision( model ) )
		{
			verdict.reason = AR_STATIC_COLLISION;
			return;
		}
		const int count = AcousticMesh_CountModelTris( model, AcousticMesh_EntitySkin( epairs ) );
		if( count == 0 )
		{
			verdict.reason = AR_NO_ACOUSTIC_SURFACE;
		}
		else if( count > budget )
		{
			verdict.reason = AR_STATIC_OVER_BUDGET;
			verdict.detail.Format( "%i > %i", count, budget );
			AcousticMesh_Warn( build, "entity %i '%s': static over budget, %i > %i triangles", entity, name.c_str(), count, budget );
		}
		else
		{
			verdict.reason = AR_STATIC_TRIANGLES;
		}
		return;
	}

	idStr spawnclass;
	AcousticMesh_ResolvedKey( build, entity, "spawnclass", spawnclass );
	if( !spawnclass.Icmp( "idDoor" ) )
	{
		verdict.reason = AR_DOOR_PENDING;	// SB1a-D: screen, record, plug; a door's geometry is never in the mesh
		return;
	}
	if( mapEnt->GetNumPrimitives() > 0 || !modelName.IsEmpty() )
	{
		verdict.reason = AR_NOT_STATIONARY;
		verdict.detail = spawnclass;
		return;
	}
	verdict.reason = AR_NO_GEOMETRY;
}

/*
============
AcousticMesh_EmitEntity

(c) for one entity that is in: R-SB1a-18, R-SB1a-19, R-SB1a-22, R-SB1a-33 in R-SB1a-23's order, then R-SB1a-13 and
R-SB1a-38. Returns the triangles it kept.
============
*/
static int AcousticMesh_EmitEntity( acousticBuild_t& build, int entity, idList<const uBrush_t*>& byPrimitive )
{
	acousticVerdict_t& verdict = build.verdicts[entity];
	const idMapEntity* mapEnt = build.mesh.mapFile->GetEntity( entity );
	const idDict& epairs = mapEnt->epairs;
	const int firstTri = build.mesh.tris.Num();

	if( verdict.reason == AR_WORLD || verdict.reason == AR_STATIC_BRUSHES )
	{
		acousticXform_t xf;
		AcousticMesh_SetXform( entity, true, xf );
		const idDeclSkin* skin = xf.world ? NULL : AcousticMesh_EntitySkin( epairs );
		const int firstFull = build.mesh.staticTris.Num();	//puncho - SB1b-P rule 6: its patches and polygon meshes
		const int firstStatic = build.mesh.statics.Num();

		AcousticMesh_GatherBrushes( entity, byPrimitive );
		idList<int> brushTris;		// R-SB1a-38: the triangles each brush put into the mesh, by map primitive
		brushTris.AssureSize( byPrimitive.Num(), 0 );
		for( int p = 0; p < mapEnt->GetNumPrimitives(); p++ )
		{
			const idMapPrimitive* prim = mapEnt->GetPrimitive( p );
			if( prim->GetType() == idMapPrimitive::TYPE_BRUSH )
			{
				if( byPrimitive[p] != NULL )
				{
					const int before = build.mesh.tris.Num();
					AcousticMesh_EmitBrush( build, byPrimitive[p], xf, skin );
					brushTris[p] = build.mesh.tris.Num() - before;
				}
			}
			else if( prim->GetType() == idMapPrimitive::TYPE_PATCH || prim->GetType() == idMapPrimitive::TYPE_MESH )
			{
				const int beforeTris = build.mesh.tris.Num();
				const int beforeFull = build.mesh.staticTris.Num();
				if( prim->GetType() == idMapPrimitive::TYPE_MESH )
				{
					AcousticMesh_EmitPolygonMesh( build, static_cast<const MapPolygonMesh*>( prim ), xf, skin );
				}
				else if( !dmapGlobals.noCurves )
				{
					AcousticMesh_EmitPatch( build, static_cast<const idMapPatch*>( prim ), xf, skin );
				}
				//puncho - SB1b-P (rule 6, review P4S-2): one record per patch or polygon mesh, emit order, when it put a
				// triangle into the mesh; else its full list goes too (it blocks nothing)
				if( build.mesh.tris.Num() > beforeTris && build.mesh.staticTris.Num() > beforeFull )
				{
					acousticStatic_t record;
					record.entity = entity;
					record.firstTri = beforeFull;
					record.numTris = build.mesh.staticTris.Num() - beforeFull;
					build.mesh.statics.Append( record );
				}
				else
				{
					build.mesh.staticTris.SetNum( beforeFull );
				}
			}
		}

		const int numTris = build.mesh.tris.Num() - firstTri;
		if( verdict.reason == AR_STATIC_BRUSHES )
		{
			if( numTris == 0 )
			{
				verdict.reason = AR_NO_ACOUSTIC_SURFACE;	// R-SB1a-13: a missing obstacle, never a leak
				build.mesh.staticTris.SetNum( firstFull );	//puncho - SB1b-P: out of the mesh, blocks nothing (rule 6)
				build.mesh.statics.SetNum( firstStatic );
				return 0;
			}
			AcousticMesh_EmitBrushPlanes( build, entity, byPrimitive, brushTris, xf );
		}
		return numTris;
	}

	if( verdict.reason == AR_INLINED || verdict.reason == AR_STATIC_COLLISION || verdict.reason == AR_STATIC_TRIANGLES )
	{
		acousticXform_t xf;
		AcousticMesh_SetXform( entity, false, xf );
		const int firstFull = build.mesh.staticTris.Num();

		const char* modelName = epairs.GetString( "model" );
		if( verdict.reason == AR_INLINED )
		{
			// as dmap inlined it: no skin, no budget, no extension test
			const idRenderModel* model = renderModelManager->FindModel( modelName );
			AcousticMesh_EmitModel( build, model, xf, NULL, false, build.mesh.mapFile->GetEntity( 0 )->epairs.GetBool( "inlineAllStatics" ) );
		}
		else
		{
			const idRenderModel* model = renderModelManager->CheckModel( modelName );
			AcousticMesh_EmitModel( build, model, xf, AcousticMesh_EntitySkin( epairs ), verdict.reason == AR_STATIC_COLLISION, false );
		}

		const int numTris = build.mesh.tris.Num() - firstTri;
		if( numTris == 0 )
		{
			verdict.reason = AR_NO_ACOUSTIC_SURFACE;
			build.mesh.staticTris.SetNum( firstFull );	//puncho - SB1b-P: a static out of the mesh blocks nothing (rule 6)
		}
		else
		{
			//puncho - SB1b-P (rule 6): the static is in the mesh; its whole list, map order
			acousticStatic_t record;
			record.entity = entity;
			record.firstTri = firstFull;
			record.numTris = build.mesh.staticTris.Num() - firstFull;
			build.mesh.statics.Append( record );
		}
		return numTris;
	}

	return 0;
}

/*
============
AcousticMesh_Check

(h) R-SB1a-31's invariants. false names the first breach in cause.
============
*/
static bool AcousticMesh_Check( const acousticBuild_t& build, idStr& cause )
{
	const acousticMesh_t& mesh = build.mesh;

	// the worldspawn's own triangles: numWorldTris counts every non-plug triangle, statics included, so it could pass
	// with no world at all; the world's count being > 0 implies numWorldTris > 0
	if( build.verdicts[0].numTris <= 0 )
	{
		cause = "no world triangle";
		return false;
	}
	if( mesh.materials.Num() > 65535 )
	{
		cause.Format( "%i materials, more than 65,535", mesh.materials.Num() );
		return false;
	}
	for( int t = 0; t < mesh.tris.Num(); t++ )
	{
		const acousticTri_t& tri = mesh.tris[t];
		for( int k = 0; k < 3; k++ )
		{
			if( tri.v[k] < 0 || tri.v[k] >= mesh.verts.Num() )
			{
				cause.Format( "triangle %i: vertex %i out of range", t, tri.v[k] );
				return false;
			}
		}
		if( tri.material >= mesh.materials.Num() )
		{
			cause.Format( "triangle %i: material %i out of range", t, tri.material );
			return false;
		}
		const bool plug = ( tri.flags & 1 ) != 0;
		if( plug != ( t >= mesh.numWorldTris ) )
		{
			cause.Format( "triangle %i: plugs are not exactly [%i, %i)", t, mesh.numWorldTris, mesh.tris.Num() );
			return false;
		}
	}
	for( int b = 0; b < mesh.brushes.Num(); b++ )
	{
		const acousticBrush_t& brush = mesh.brushes[b];
		if( brush.firstPlane < 0 || brush.numPlanes < 0 || brush.firstPlane + brush.numPlanes > mesh.brushPlanes.Num() )
		{
			cause.Format( "brush record %i: planes outside brushPlanes", b );
			return false;
		}
	}
	if( mesh.portals.Num() != interAreaPortals.Num() )
	{
		cause.Format( "%i portal records for %i interAreaPortals", mesh.portals.Num(), interAreaPortals.Num() );
		return false;
	}
	for( int p = 0; p < mesh.portals.Num(); p++ )
	{
		const acousticPortal_t& portal = mesh.portals[p];
		if( portal.area0 != interAreaPortals[p].area0 || portal.area1 != interAreaPortals[p].area1 )
		{
			cause.Format( "portal %i: areas differ from interAreaPortals", p );
			return false;
		}
		if( portal.door < -1 || portal.door >= mesh.doors.Num() )
		{
			cause.Format( "portal %i: door %i out of range", p, portal.door );
			return false;
		}
	}
	for( int d = 0; d < mesh.doors.Num(); d++ )
	{
		const acousticDoor_t& door = mesh.doors[d];
		if( door.portal < 0 || door.portal >= mesh.portals.Num() )
		{
			cause.Format( "door %i: portal %i out of range", d, door.portal );
			return false;
		}
		if( door.numPlugTris < 0 || ( door.numPlugTris > 0 && ( door.firstPlugTri < mesh.numWorldTris || door.firstPlugTri + door.numPlugTris > mesh.tris.Num() ) ) )
		{
			cause.Format( "door %i: plug range outside the plugs", d );
			return false;
		}
		if( door.material < -1 || door.material >= mesh.materials.Num() )
		{
			cause.Format( "door %i: material %i out of range", d, door.material );
			return false;
		}
		//puncho - SB1a-D (review M3-16): each door against its portal's owner, not every earlier door; equal to the owner
		// means every door on the portal shares one range
		const int owner = mesh.portals[door.portal].door;
		if( owner < 0 || owner > d )
		{
			cause.Format( "door %i: portal %i is owned by door %i", d, door.portal, owner );
			return false;
		}
		if( mesh.doors[owner].portal != door.portal || mesh.doors[owner].firstPlugTri != door.firstPlugTri || mesh.doors[owner].numPlugTris != door.numPlugTris )
		{
			cause.Format( "doors %i and %i on portal %i do not share a range", owner, d, door.portal );
			return false;
		}
	}
	if( mesh.entities.Num() != mesh.mapFile->GetNumEntities() )
	{
		cause.Format( "%i entity records for %i entities", mesh.entities.Num(), mesh.mapFile->GetNumEntities() );
		return false;
	}
	for( int i = 0; i < mesh.entities.Num(); i++ )
	{
		if( mesh.entities[i].entity != i )
		{
			cause.Format( "entity record %i names entity %i", i, mesh.entities[i].entity );
			return false;
		}
		if( build.verdicts[i].reason == AR_DOOR_PENDING )
		{
			cause.Format( "entity %i '%s': %s, the door screen is not built", i, mesh.entities[i].name.c_str(), AcousticReasonName( AR_DOOR_PENDING ) );
			return false;
		}
	}
	return true;
}

// review M2-8: a failure in (a) to (h) or (j) leaves no acoustic output, so no old bake outlives its map ((k) removes
// only the manifest, M2R-2). The
// region guard is the caller's: dmap.cpp calls AcousticStageA only when !region, as it guards its own delete
static bool AcousticMesh_FailNoOutputs( const char* cause )
{
	common->Printf( "acoustics: Stage A failed: %s\n", cause );
	AcousticDeleteOutputs( dmapGlobals.mapFileBase );
	return false;
}

// a failure of (a) to (h): the phase is timed, the cause printed, the outputs removed
static bool AcousticMesh_Fail( int startMs, const char* cause )
{
	AcousticPhaseEnd( DP_ACOUSTIC_MESH, startMs );
	return AcousticMesh_FailNoOutputs( cause );
}

//puncho - SB1a-D begin
// R-SB1a-16: every portal's points as the game reads them back from the .proc's text, and their bounds
struct acousticPortalPoints_t
{
	idList<idVec3>			points;
	idList<int>				first;		// numPortals + 1 entries: portal p owns points [first[p], first[p + 1])
	idList<idBounds>		bounds;
};

// a door whose box is not mirrored: the default cube, its kept geometry, the box of its mins/maxs or size keys
static const int ACOUSTIC_DOOR_VOID_BOXES = 3;

// one door in map order between the screen and the records
struct acousticDoorScreen_t
{
	int						entity;
	int						portal;		// the lookup's portal, -1 none (no box, or no portal met)
	bool					passed;		// D1 to D5 all passed
	const idMaterial*		material;	// R-SB1a-17's plug material; NULL is _acoustic_generic
	idBounds				voidBoxes[ACOUSTIC_DOOR_VOID_BOXES];	// a door whose box is not mirrored: each place it
	int						numVoidBoxes;							// may stand, tested on its own (M3R-2, M3S-1)
};

/*
============
AcousticMesh_ReadBackPortals

R-SB1a-16: each portal's winding written with dmap's own Write1DMatrix, as WriteOutputPortals writes it, then read
back with the lexer flags and Parse1DMatrix the game's ParseInterAreaPortals uses: the points are the game's,
not the winding's floats (a wall sloped 1/2048 prints with decimals). One text write and one parse for every portal.
============
*/
static bool AcousticMesh_ReadBackPortals( acousticPortalPoints_t& out )
{
	const int numPortals = interAreaPortals.Num();
	idFile_Memory text;
	int numPoints = 0;
	for( int p = 0; p < numPortals; p++ )
	{
		const idWinding* w = AcousticMesh_PortalWinding( p );
		for( int j = 0; j < w->GetNumPoints(); j++ )
		{
			float xyz[3] = { ( *w )[j].x, ( *w )[j].y, ( *w )[j].z };	// Write1DMatrix takes a non-const pointer
			Write1DMatrix( &text, 3, xyz );
		}
		numPoints += w->GetNumPoints();
	}

	out.points.SetNum( numPoints );
	out.first.SetNum( numPortals + 1 );
	out.bounds.SetNum( numPortals );

	// the game's flags, and no fatal error: a bad point fails Stage A here, which removes the outputs (review M3-9)
	idLexer lexer( LEXFL_NOSTRINGCONCAT | LEXFL_NODOLLARPRECOMPILE | LEXFL_NOFATALERRORS | LEXFL_NOERRORS );
	if( numPoints > 0 && !lexer.LoadMemory( text.GetDataPtr(), text.Length(), "acoustic portal points" ) )
	{
		common->Printf( "acoustics: the portal points could not be read back\n" );
		return false;
	}
	int n = 0;
	for( int p = 0; p < numPortals; p++ )
	{
		out.first[p] = n;
		out.bounds[p].Clear();
		const int count = AcousticMesh_PortalWinding( p )->GetNumPoints();
		for( int j = 0; j < count; j++, n++ )
		{
			if( !lexer.Parse1DMatrix( 3, out.points[n].ToFloatPtr() ) || lexer.HadError() )	// ParseFloat reports only through Error
			{
				common->Printf( "acoustics: portal %i point %i could not be read back\n", p, j );
				return false;
			}
			out.bounds[p].AddPoint( out.points[n] );
		}
	}
	out.first[numPortals] = n;
	return true;
}

// LoadRenderModel's extensions: the render models the collision loader converts (R-SB1a-12's list plus dae)
static bool AcousticMesh_CollisionFormat( const char* modelName )
{
	static const char* const extensions[] = { "glb", "gltf", "ase", "lwo", "ma", "dae", "obj" };
	idStr extension;
	idStr( modelName ).ExtractFileExtension( extension );
	const int numExtensions = sizeof( extensions ) / sizeof( extensions[0] );
	for( int i = 0; i < numExtensions; i++ )
	{
		if( !extension.Icmp( extensions[i] ) )
		{
			return true;
		}
	}
	return false;
}

/*
============
AcousticMesh_PlaceDoorBox

Clip.cpp's absBounds of a clip model with local bounds local at position, under axis: rotated only when the axis
is, grown by CM_BOX_EPSILON.
============
*/
static idBounds AcousticMesh_PlaceDoorBox( const idBounds& local, const idVec3& position, const idMat3& axis )
{
	idBounds box;
	if( axis.IsRotated() )
	{
		box.FromTransformedBounds( local, position, axis );
	}
	else
	{
		box[0] = local[0] + position;
		box[1] = local[1] + position;
	}
	const idVec3 epsilon( CM_BOX_EPSILON, CM_BOX_EPSILON, CM_BOX_EPSILON );
	box[0] -= epsilon;
	box[1] += epsilon;
	return box;
}

/*
============
AcousticMesh_DoorBox

R-SB1a-15: the door's GetAbsBounds() at spawn, from the local bounds of the clip model the game builds for it
(Entity.cpp InitDefaultPhysics, cm/CollisionModel_load.cpp), placed at origin + pivot, rotated only when the game
rotates it (with movedir, the spawn axis turned to angles and back by Physics_Parametric's SetAxis; without, none),
grown by CM_BOX_EPSILON as Clip.cpp grows it. true: box is the mirrored box. model: a model door's model, else
NULL. byPrimitive: a brush door's brushes by map primitive, read again by AcousticMesh_DoorMaterial.

false: the box is not mirrored (D4); why is the manifest's detail, or NULL; voidBoxes[0, numVoidBoxes) are the
places the door may still stand, for R-SB1a-17, each tested on its own (review M3R-2, M3S-1, M3S-2): the default
clip model's box, the door's own kept geometry, and the box its mins/maxs or size keys give the game. Still not
covered: a clipmodel naming another model (its box is that model's, never gathered); a clipmodel naming a model
that fails the game's CheckModel, where the game falls back to noclipmodel, the keys and the model key; and a .cm
beside the model whose polygons reach past the kept surfaces. The keys' box is added only where the game reads the
keys, which is where the verdict chain stopped on them (why is "mins" or "size", review M3T-1). A door with no clip model of its own is not boxless in the game: idPhysics_Static::GetClipModel returns
gameLocal.clip.DefaultClipModel(), whose trace model is idBounds( vec3_origin ).Expand( 8 ) (Clip.cpp), and
idMover_Binary::Spawn copies that clip model (Mover.cpp), so FindPortal gets the door's origin (plus the pivot when
it has a model key) +-8, grown by 1: +-9. That covers no model key, noclipmodel, a model the collision loader
refuses (md5mesh, missing), and a model with no kept surface (zeroed bounds, inside the cube).

The clip model is taken, as Entity.cpp takes it: the clipmodel key first, outside the noclipmodel test, and when it
equals the model key it is exactly the mirrored clip model; else (clipmodel empty) noclipmodel, mins/maxs, size, and
then the model key. Only the model key's own clip model is mirrored; any other source is D4 (review M3-2, M3-5,
M3R-4). The collision model loader keeps, of a map entity (LoadModel from the map's primitives):
- brushes: ConvertBrush ORs contents (CONTENTS_REMOVE_UTIL) over sides 0 to n-2 only, and a brush with any is a
  brush with its full bounds; ConvertBrushSides adds a polygon for every side with contents, so a brush whose only
  side with contents is its last adds that side's winding alone (review M3R-5). Planes translated by -originOffset.
- patches whose material has CONTENTS_REMOVE_UTIL, tessellated, points as parsed (ConvertPatch takes no offset);
- meshes, every polygon, points as parsed (ConvertMesh takes no offset, and no contents test).
Of a render model (LoadRenderModel): the surfaces whose material has CONTENTS_REMOVE_UTIL, only the SURF_COLLISION
ones when the model has any. Not mirrored, so D4 `collision file`: a <model>.cm beside the model, which LoadModel
takes first. Not mirrored and not tested (review M3-1, M3R-6): the generated caches generated/collision/<model>.bcmodel
and generated/<model>.bcm; one made from the render model holds the same surfaces, but one left from a .cm since
deleted, or stale against its timestamp check, would give the game another box.
============
*/
static bool AcousticMesh_DoorBox( acousticBuild_t& build, int entity, idList<const uBrush_t*>& byPrimitive, const idRenderModel*& model, idBounds& box, const char*& why, idBounds* voidBoxes, int& numVoidBoxes )
{
	const idMapEntity* mapEnt = build.mesh.mapFile->GetEntity( entity );
	idMat3 axis;
	idVec3 origin, pivot;
	AcousticMesh_EntityTransform( entity, axis, origin, pivot );

	model = NULL;
	why = NULL;
	numVoidBoxes = 0;

	idStr clipName, modelName, value, movedir;
	AcousticMesh_ResolvedKey( build, entity, "clipmodel", clipName );
	AcousticMesh_ResolvedKey( build, entity, "model", modelName );
	if( !AcousticMesh_ResolvedKey( build, entity, "movedir", movedir ) )
	{
		axis.Identity();	// Mover.cpp: no movedir, SetAngles( ang_zero )
	}
	else
	{
		axis = axis.ToAngles().ToMat3();	// Physics_Parametric SetAxis keeps angles only: scale and mirroring go (M3-3)
	}

	// the game adds the pivot through the spawn dict's two-decimal text, and only with a model key (Game_local.cpp,
	// Entity.cpp; review M3-7); dmap's brush bounds come from planes snapped at 0.01 (map.cpp), the collision model's
	// at 1e-4: an edge-only difference, left
	idVec3 position = origin;
	if( !modelName.IsEmpty() )
	{
		idDict pivotText;
		pivotText.SetVector( "origin_offset", pivot );
		position += pivotText.GetVector( "origin_offset" );
	}

	// the door's own geometry, as the collision loader keeps it; local to origin + pivot
	idBounds local;
	local.Clear();
	if( mapEnt->GetNumPrimitives() > 0 )
	{
		AcousticMesh_GatherBrushes( entity, byPrimitive );
		for( int p = 0; p < mapEnt->GetNumPrimitives(); p++ )
		{
			const idMapPrimitive* prim = mapEnt->GetPrimitive( p );
			if( prim->GetType() == idMapPrimitive::TYPE_BRUSH )
			{
				const idMapBrush* mapBrush = static_cast<const idMapBrush*>( prim );
				const uBrush_t* brush = byPrimitive[p];
				const int numSides = mapBrush->GetNumSides();
				if( brush == NULL || numSides < 1 )
				{
					continue;
				}
				int contents = 0;
				for( int s = 0; s < numSides - 1; s++ )
				{
					contents |= declManager->FindMaterial( mapBrush->GetSide( s )->GetMaterial() )->GetContentFlags() & CONTENTS_REMOVE_UTIL;
				}
				if( contents != 0 )
				{
					local.AddPoint( brush->bounds[0] - pivot );
					local.AddPoint( brush->bounds[1] - pivot );
				}
				else if( declManager->FindMaterial( mapBrush->GetSide( numSides - 1 )->GetMaterial() )->GetContentFlags() & CONTENTS_REMOVE_UTIL )
				{
					// the last side's polygon alone; dmap keeps the map's sides in order unless it dropped one, and then
					// the whole brush stands in for it
					const idWinding* w = ( brush->numsides == numSides ) ? brush->sides[numSides - 1].winding : NULL;
					if( w != NULL )
					{
						for( int k = 0; k < w->GetNumPoints(); k++ )
						{
							local.AddPoint( ( *w )[k].ToVec3() - pivot );
						}
					}
					else
					{
						local.AddPoint( brush->bounds[0] - pivot );
						local.AddPoint( brush->bounds[1] - pivot );
					}
				}
			}
			else if( prim->GetType() == idMapPrimitive::TYPE_PATCH )
			{
				const idMapPatch* patch = static_cast<const idMapPatch*>( prim );
				if( !( declManager->FindMaterial( patch->GetMaterial() )->GetContentFlags() & CONTENTS_REMOVE_UTIL ) )
				{
					continue;
				}
				idSurface_Patch cp( *patch );
				if( patch->GetExplicitlySubdivided() )
				{
					cp.SubdivideExplicit( patch->GetHorzSubdivisions(), patch->GetVertSubdivisions(), false, true );
				}
				else
				{
					cp.Subdivide( DEFAULT_CURVE_MAX_ERROR_CD, DEFAULT_CURVE_MAX_ERROR_CD, DEFAULT_CURVE_MAX_LENGTH_CD, false );
				}
				const int* indexes = cp.GetIndexes();
				for( int i = 0; i < cp.GetNumIndexes(); i++ )
				{
					local.AddPoint( cp[indexes[i]].xyz );
				}
			}
			else if( prim->GetType() == idMapPrimitive::TYPE_MESH )
			{
				const MapPolygonMesh* mesh = static_cast<const MapPolygonMesh*>( prim );
				const idList<idDrawVert>& verts = mesh->GetDrawVerts();
				for( int f = 0; f < mesh->GetNumPolygons(); f++ )
				{
					const idList<int>& indexes = mesh->GetFace( f ).GetIndexes();
					for( int i = 0; i < indexes.Num(); i++ )
					{
						local.AddPoint( verts[indexes[i]].xyz );
					}
				}
			}
		}
	}
	else if( !modelName.IsEmpty() && AcousticMesh_CollisionFormat( modelName ) && !AcousticNameSet_Contains( build.missingModels, modelName.c_str() ) )
	{
		// func_door_model: the surfaces LoadRenderModel keeps, of every format it converts; R-SB1a-12's extension rule
		// decides below whether the box is mirrored
		const idRenderModel* found = renderModelManager->CheckModel( modelName );
		if( found == NULL || found->IsDefaultModel() )
		{
			AcousticNameSet_Add( build.missingModels, modelName.c_str(), modelName.Length() );
		}
		else
		{
			model = found;
			const bool collision = AcousticMesh_ModelHasCollision( model );
			for( int i = 0; i < model->NumSurfaces(); i++ )
			{
				const modelSurface_t* surface = model->Surface( i );
				const srfTriangles_t* geometry = surface->geometry;
				if( geometry == NULL || surface->shader == NULL || !( surface->shader->GetContentFlags() & CONTENTS_REMOVE_UTIL ) )
				{
					continue;
				}
				if( collision && !AcousticMesh_IsCollisionSurface( surface ) )
				{
					continue;
				}
				for( int j = 0; j < geometry->numIndexes; j++ )
				{
					local.AddPoint( geometry->verts[geometry->indexes[j]].xyz );
				}
			}
		}
	}

	// the clip model's source, in Entity.cpp's order
	if( !clipName.IsEmpty() )
	{
		if( clipName.Icmp( modelName ) != 0 )
		{
			why = "clipmodel";
		}
	}
	else if( AcousticMesh_ResolvedKey( build, entity, "noclipmodel", value ) && atoi( value.c_str() ) != 0 )
	{
		why = "noclipmodel";
	}
	else if( AcousticMesh_ResolvedKey( build, entity, "mins", value ) && AcousticMesh_ResolvedKey( build, entity, "maxs", value ) )
	{
		why = "mins";
	}
	else if( AcousticMesh_ResolvedKey( build, entity, "size", value ) )
	{
		why = "size";
	}
	else if( modelName.IsEmpty() )
	{
		why = "no clip model";
	}
	bool mirrored = ( why == NULL );
	if( mirrored && mapEnt->GetNumPrimitives() == 0 )
	{
		idStr cmName = modelName;
		cmName.SetFileExtension( "cm" );
		if( !AcousticMesh_ModelFormatOk( modelName ) || model == NULL )
		{
			mirrored = false;	// a format the static rule refuses, or missing: the default cube
		}
		else if( fileSystem->ReadFile( cmName, NULL, NULL ) >= 0 )
		{
			why = "collision file";
			mirrored = false;
		}
		else if( local.IsCleared() )
		{
			why = "no solid surface";	// review M3R-7: the game's collision model has zeroed bounds, inside the cube
			mirrored = false;
		}
	}
	if( mirrored && local.IsCleared() )
	{
		mirrored = false;	// a brush door with nothing the loader keeps
	}

	if( mirrored )
	{
		box = AcousticMesh_PlaceDoorBox( local, position, axis );
		return true;
	}

	// review M3R-2, M3S-1, M3S-2: where the door may stand, each box on its own: the default cube at its origin, its
	// own geometry at its true place, and the trace-model box of its mins/maxs or size keys (Entity.cpp)
	voidBoxes[numVoidBoxes++] = AcousticMesh_PlaceDoorBox( idBounds( vec3_origin ).Expand( 8.0f ), position, axis );
	if( !local.IsCleared() )
	{
		voidBoxes[numVoidBoxes++] = AcousticMesh_PlaceDoorBox( local, origin + pivot, axis );
	}
	// the keys' box only where the chain above stopped on the keys: clipmodel empty, noclipmodel 0 (review M3T-1)
	idStr minsText, maxsText, sizeText;
	idDict keyText;
	if( why != NULL && !idStr::Cmp( why, "mins" ) )
	{
		AcousticMesh_ResolvedKey( build, entity, "mins", minsText );
		AcousticMesh_ResolvedKey( build, entity, "maxs", maxsText );
		keyText.Set( "mins", minsText );
		keyText.Set( "maxs", maxsText );
		voidBoxes[numVoidBoxes++] = AcousticMesh_PlaceDoorBox( idBounds( keyText.GetVector( "mins" ), keyText.GetVector( "maxs" ) ), position, axis );
	}
	else if( why != NULL && !idStr::Cmp( why, "size" ) )
	{
		AcousticMesh_ResolvedKey( build, entity, "size", sizeText );
		keyText.Set( "size", sizeText );
		const idVec3 size = keyText.GetVector( "size" );
		const idBounds sizeBounds( idVec3( size.x * -0.5f, size.y * -0.5f, 0.0f ), idVec3( size.x * 0.5f, size.y * 0.5f, size.z ) );
		voidBoxes[numVoidBoxes++] = AcousticMesh_PlaceDoorBox( sizeBounds, position, axis );
	}
	assert( numVoidBoxes <= ACOUSTIC_DOOR_VOID_BOXES );
	return false;
}

/*
============
AcousticMesh_DoorMaterial

R-SB1a-17: a brush door's largest side passing R-SB1a-21, by the area of its winding (ties: the lowest primitive,
then the lowest side); a model door's largest surface passing it, by the area of its triangles (ties: the lowest
surface); NULL is _acoustic_generic. byPrimitive is the one AcousticMesh_DoorBox just gathered for this door.
Areas within a relative 1e-4 are a tie, so two congruent faces whose float areas differ in the last bits still go
to the lower index (review M3-15).
============
*/
static const float ACOUSTIC_AREA_TIE = 1e-4f;

static const idMaterial* AcousticMesh_DoorMaterial( const idList<const uBrush_t*>& byPrimitive, const idRenderModel* model )
{
	const idMaterial* best = NULL;
	float bestArea = 0.0f;
	if( model == NULL )
	{
		for( int p = 0; p < byPrimitive.Num(); p++ )
		{
			const uBrush_t* brush = byPrimitive[p];
			if( brush == NULL )
			{
				continue;
			}
			for( int s = 0; s < brush->numsides; s++ )
			{
				const side_t* side = &brush->sides[s];
				if( side->winding == NULL || !AcousticMesh_Passes( side->material ) )
				{
					continue;
				}
				const float area = side->winding->GetArea();
				if( area > bestArea * ( 1.0f + ACOUSTIC_AREA_TIE ) )
				{
					best = side->material;
					bestArea = area;
				}
			}
		}
		return best;
	}

	for( int i = 0; i < model->NumSurfaces(); i++ )
	{
		const modelSurface_t* surface = model->Surface( i );
		const srfTriangles_t* geometry = surface->geometry;
		if( geometry == NULL || !AcousticMesh_Passes( surface->shader ) )
		{
			continue;
		}
		float area = 0.0f;
		for( int j = 0; j + 2 < geometry->numIndexes; j += 3 )
		{
			const idVec3& a = geometry->verts[geometry->indexes[j + 0]].xyz;
			const idVec3& b = geometry->verts[geometry->indexes[j + 1]].xyz;
			const idVec3& c = geometry->verts[geometry->indexes[j + 2]].xyz;
			area += 0.5f * ( b - a ).Cross( c - a ).Length();
		}
		if( area > bestArea * ( 1.0f + ACOUSTIC_AREA_TIE ) )
		{
			best = surface->shader;
			bestArea = area;
		}
	}
	return best;
}

/*
============
AcousticMesh_Doors

(e) R-SB1a-14 to R-SB1a-17, R-SB1a-25. Pass 1, map order: screen every door left AR_DOOR_PENDING (D1 to D5,
first failure wins) and give it the portal the game's FindPortal gives it; a door that failed D1 to D3 is still
looked up, because it keeps its portal open. Pass 2: a portal any of whose doors failed is open air. Pass 3, map
order: a record for every passing door on a sealable portal; the first in map order owns the portal and its plug.
A door's own brushes and model never reach the mesh: only the plug, on the portal's read-back points.
============
*/
static bool AcousticMesh_Doors( acousticBuild_t& build )
{
	acousticMesh_t& mesh = build.mesh;
	const int numEntities = mesh.mapFile->GetNumEntities();
	const int numPortals = mesh.portals.Num();

	idList<acousticDoorScreen_t> screens;
	for( int e = 0; e < numEntities; e++ )
	{
		if( build.verdicts[e].reason == AR_DOOR_PENDING )
		{
			acousticDoorScreen_t& screen = screens.Alloc();
			screen.entity = e;
			screen.portal = -1;
			screen.passed = false;
			screen.material = NULL;
			screen.numVoidBoxes = 0;
		}
	}
	if( screens.Num() == 0 )
	{
		return true;	// no door: no read-back, no cost
	}

	acousticPortalPoints_t portalPoints;
	if( !AcousticMesh_ReadBackPortals( portalPoints ) )
	{
		return false;
	}

	// pass 1: the screen and the lookup
	static const char* const inhibitKeys[] = { "not_multiplayer", "not_easy", "not_medium", "not_hard", "not_nightmare" };
	static const char* const dungeonKeys[] = { "dungeon_if", "dungeon_unless", "dungeon_variant" };
	const int numInhibitKeys = sizeof( inhibitKeys ) / sizeof( inhibitKeys[0] );
	const int numDungeonKeys = sizeof( dungeonKeys ) / sizeof( dungeonKeys[0] );
	idList<const uBrush_t*> byPrimitive;
	for( int d = 0; d < screens.Num(); d++ )
	{
		acousticDoorScreen_t& screen = screens[d];
		const int entity = screen.entity;
		acousticVerdict_t& verdict = build.verdicts[entity];
		const idDict& epairs = mesh.mapFile->GetEntity( entity )->epairs;

		int reason = AR_DOOR_PENDING;
		idStr value;

		// D1: start_open resolved, read as idDict::GetBool reads it
		if( AcousticMesh_ResolvedKey( build, entity, "start_open", value ) && atoi( value.c_str() ) != 0 )
		{
			reason = AR_DOOR_START_OPEN;
		}
		// D2: a map key only; the door binds before start_open acts
		if( reason == AR_DOOR_PENDING && epairs.FindKey( "bind" ) != NULL )
		{
			reason = AR_DOOR_BIND;
		}
		// D3: map keys only
		for( int k = 0; reason == AR_DOOR_PENDING && k < numInhibitKeys; k++ )
		{
			const char* v = epairs.GetString( inhibitKeys[k] );
			if( v[0] != '\0' && idStr::Cmp( v, "0" ) != 0 )
			{
				reason = AR_DOOR_INHIBIT_KEY;
				verdict.detail = inhibitKeys[k];
			}
		}
		for( int k = 0; reason == AR_DOOR_PENDING && k < numDungeonKeys; k++ )
		{
			if( epairs.GetString( dungeonKeys[k] )[0] != '\0' )
			{
				reason = AR_DOOR_INHIBIT_KEY;
				verdict.detail = dungeonKeys[k];
			}
		}

		// D4: the box
		const idRenderModel* model;
		idBounds box;
		const char* why;
		const bool hasBox = AcousticMesh_DoorBox( build, entity, byPrimitive, model, box, why, screen.voidBoxes, screen.numVoidBoxes );
		if( !hasBox )
		{
			if( reason == AR_DOOR_PENDING )
			{
				reason = AR_DOOR_MODEL_BOX_UNKNOWN;
				if( why != NULL )
				{
					verdict.detail = why;
				}
			}
		}
		else
		{
			// D5: the first portal whose read-back bounds meet the box, as FindPortal, which does not skip; a winding
			// of fewer than 3 points bounds no doorway, so a door that meets one first has no portal (review M3R-3)
			for( int p = 0; p < numPortals; p++ )
			{
				if( portalPoints.bounds[p].IntersectsBounds( box ) )
				{
					screen.portal = p;
					break;
				}
			}
			const bool degenerate = ( screen.portal != -1 && portalPoints.first[screen.portal + 1] - portalPoints.first[screen.portal] < 3 );
			if( ( screen.portal == -1 || degenerate ) && reason == AR_DOOR_PENDING )
			{
				reason = AR_DOOR_NO_PORTAL;
				screen.portal = -1;
				AcousticMesh_Warn( build, "entity %i '%s': door no portal", entity, epairs.GetString( "name" ) );
			}
		}

		if( reason == AR_DOOR_PENDING )
		{
			screen.passed = true;
			screen.material = AcousticMesh_DoorMaterial( byPrimitive, model );
		}
		else
		{
			verdict.reason = reason;
		}
	}

	// pass 2: per portal, the first door in map order that is on it and failed; -1 none. A failed door whose box is
	// not mirrored may stand on any portal one of its void boxes meets, each box tested on its own (M3S-1): no plug
	// where the compiler cannot know which door holds a portal (review M3R-2). The ruled trade (M3S-4): where such a
	// door in fact closes a portal in the game, that doorway is still baked open, never plugged by a guess
	idList<int> failing;
	failing.AssureSize( numPortals, -1 );
	for( int d = 0; d < screens.Num(); d++ )
	{
		const acousticDoorScreen_t& screen = screens[d];
		if( screen.passed )
		{
			continue;
		}
		if( screen.portal != -1 && failing[screen.portal] == -1 )
		{
			failing[screen.portal] = screen.entity;
		}
		for( int v = 0; v < screen.numVoidBoxes; v++ )
		{
			for( int p = 0; p < numPortals; p++ )
			{
				if( failing[p] == -1 && portalPoints.bounds[p].IntersectsBounds( screen.voidBoxes[v] ) )
				{
					failing[p] = screen.entity;
				}
			}
		}
	}

	// pass 3: records and plugs, map order; plug triangles after every world triangle, grouped by owning door
	const bool seal = ( mesh.settings.sealDoorways != 0 );
	for( int d = 0; d < screens.Num(); d++ )
	{
		const acousticDoorScreen_t& screen = screens[d];
		if( !screen.passed )
		{
			continue;
		}
		const int p = screen.portal;
		acousticVerdict_t& verdict = build.verdicts[screen.entity];
		if( failing[p] != -1 )
		{
			verdict.reason = AR_DOOR_SHARES_PORTAL;
			verdict.detail.Format( "portal %i entity %i", p, failing[p] );
			continue;
		}

		acousticDoor_t record;
		record.name = mesh.mapFile->GetEntity( screen.entity )->epairs.GetString( "name" );
		record.entity = screen.entity;
		record.portal = p;
		record.area0 = mesh.portals[p].area0;
		record.area1 = mesh.portals[p].area1;
		record.material = AcousticMesh_AddMaterial( build, screen.material );	// -1 past 65,535: Stage A fails after (e)

		if( mesh.portals[p].door == -1 )
		{
			// the owner: the plug is a fan of the portal's points as read back (SEAMS D-4), copied out of
			// portalPoints, never out of mesh.verts, which a weld may move (SEAMS D-27); air on both sides, flag bit 0
			record.firstPlugTri = mesh.tris.Num();
			record.numPlugTris = 0;
			const int first = portalPoints.first[p];
			const int count = portalPoints.first[p + 1] - first;
			if( seal && record.material >= 0 )
			{
				for( int k = 1; k + 1 < count; k++ )
				{
					const idVec3& a = portalPoints.points[first];
					const idVec3& b = portalPoints.points[first + k];
					const idVec3& c = portalPoints.points[first + k + 1];
					const idVec3 normal = ( c - a ).Cross( b - a );
					if( normal.x == 0.0f && normal.y == 0.0f && normal.z == 0.0f )
					{
						continue;	// collinear or repeated read-back points: no area, as dmap drops its own (review M3-12)
					}
					AcousticMesh_AddTri( build, a, b, c, record.material, 1 );
					record.numPlugTris++;
				}
			}
			mesh.portals[p].door = mesh.doors.Num();
			verdict.numTris = record.numPlugTris;
		}
		else
		{
			// a second door on the portal (a team half) shares the owner's range; its entity line shows 0 (SEAMS D-5)
			record.firstPlugTri = mesh.doors[mesh.portals[p].door].firstPlugTri;
			record.numPlugTris = mesh.doors[mesh.portals[p].door].numPlugTris;
			verdict.numTris = 0;
		}
		verdict.reason = seal ? AR_DOOR_PLUG : AR_DOOR_UNSEALED;
		mesh.doors.Append( record );
	}
	return true;
}

// one manifest line: written to the file and, under dmap_acousticVerbose, to the console
static void AcousticMesh_ManifestLine( idFile* f, bool verbose, const char* fmt, ... )
{
	char		text[4096];
	va_list		argptr;

	va_start( argptr, fmt );
	idStr::vsnPrintf( text, sizeof( text ), fmt, argptr );
	va_end( argptr );

	f->Write( text, idStr::Length( text ) );
	if( verbose )
	{
		common->Printf( "%s", text );
	}
}

// one manifest field: "-" when empty, a tab inside a name or key value turned into a space so the columns hold (M3-14)
static const char* AcousticMesh_Field( idStr& scratch, const char* text )
{
	if( text == NULL || text[0] == '\0' )
	{
		return "-";
	}
	scratch = text;
	scratch.Replace( "\t", " " );
	return scratch.c_str();
}

/*
============
AcousticMesh_WriteDump

R-SB1a-28: maps/<map>_acoustic.obj, one fixed float format, no time, no path: two compiles give equal bytes. A
diagnostic: a failure warns and fails nothing.
============
*/
static void AcousticMesh_WriteDump( const acousticBuild_t& build )
{
	const acousticMesh_t& mesh = build.mesh;
	idStrStatic< MAX_OSPATH > path;
	path.Format( "%s_acoustic.obj", dmapGlobals.mapFileBase );
	idFile* file = fileSystem->OpenFileWrite( path, "fs_basepath" );
	if( file == NULL )
	{
		common->Warning( "acoustics: cannot open %s", path.c_str() );
		return;
	}
	idFile_WriteBuffer buffer;
	buffer.Init( file );

	for( int i = 0; i < mesh.verts.Num(); i++ )
	{
		const idVec3& v = mesh.verts[i];
		buffer.WriteFloatString( "v %.6f %.6f %.6f\n", v.x, v.y, v.z );
	}
	buffer.WriteFloatString( "g world\n" );
	for( int t = 0; t < mesh.numWorldTris; t++ )
	{
		const acousticTri_t& tri = mesh.tris[t];
		buffer.WriteFloatString( "f %i %i %i\n", tri.v[0] + 1, tri.v[1] + 1, tri.v[2] + 1 );
	}
	for( int d = 0; d < mesh.doors.Num(); d++ )
	{
		const acousticDoor_t& door = mesh.doors[d];
		if( mesh.portals[door.portal].door != d || door.numPlugTris == 0 )
		{
			continue;	// a plug is dumped once, under the door that owns it
		}
		if( door.name.IsEmpty() )
		{
			buffer.WriteFloatString( "g plug_%i\n", door.entity );
		}
		else
		{
			buffer.WriteFloatString( "g plug_%s\n", door.name.c_str() );
		}
		for( int t = door.firstPlugTri; t < door.firstPlugTri + door.numPlugTris; t++ )
		{
			const acousticTri_t& tri = mesh.tris[t];
			buffer.WriteFloatString( "f %i %i %i\n", tri.v[0] + 1, tri.v[1] + 1, tri.v[2] + 1 );
		}
	}

	if( !buffer.Close() )
	{
		common->Warning( "acoustics: error writing %s", path.c_str() );
	}
}

/*
============
AcousticMesh_WriteOutputs

(k) R-SB1a-26 to R-SB1a-28: the manifest through a buffered file on fs_basepath, as WriteOutputFile writes the
.proc (a failed write removes the partial file, so no half manifest is ever left); then the console line; then
the dump. false: the manifest was not written, which fails Stage A.
============
*/
static bool AcousticMesh_WriteOutputs( const acousticBuild_t& build )
{
	const acousticMesh_t& mesh = build.mesh;
	const bool verbose = dmap_acousticVerbose.GetBool();

	// the old dump goes before anything can return, so none outlives this compile (review M3-10, M3R-8)
	fileSystem->RemoveFile( va( "%s_acoustic.obj", dmapGlobals.mapFileBase ) );

	idStrStatic< MAX_OSPATH > path;
	path.Format( "%s.sndmanifest", dmapGlobals.mapFileBase );
	idFile* file = fileSystem->OpenFileWrite( path, "fs_basepath" );
	if( file == NULL )
	{
		common->Warning( "acoustics: cannot open %s", path.c_str() );
		return false;
	}
	idFile_WriteBuffer buffer;
	buffer.Init( file );

	AcousticMesh_ManifestLine( &buffer, verbose, "sndmanifest\t1\trevisionA\t%i\n", SND_BAKER_REVISION_A );
	// lower-cased, so the bytes do not depend on how the name was typed (review M3-14)
	idStr mapName = dmapGlobals.mapFileBase;
	mapName.ToLower();
	idStr fieldA, fieldB, fieldC;
	AcousticMesh_ManifestLine( &buffer, verbose, "map\t%s\n", AcousticMesh_Field( fieldA, mapName.c_str() ) );
	AcousticMesh_ManifestLine( &buffer, verbose, "settings\tsealDoorways\t%i\tstaticTriBudget\t%i\tscriptCRC\t%08x\n",
							   mesh.settings.sealDoorways, mesh.settings.staticTriBudget, ( unsigned int )mesh.scriptCRC );

	int numIn = 0;
	int numOut = 0;
	for( int e = 0; e < build.verdicts.Num(); e++ )
	{
		const acousticVerdict_t& verdict = build.verdicts[e];
		const idDict& epairs = mesh.mapFile->GetEntity( e )->epairs;
		const char* name = epairs.GetString( "name" );
		const char* classname = epairs.GetString( "classname" );
		const int kind = AcousticReasonKind( verdict.reason );
		if( kind == 2 )
		{
			numIn++;
		}
		else
		{
			numOut++;
		}
		AcousticMesh_ManifestLine( &buffer, verbose, "entity\t%i\t%s\t%s\t%s\t%i\t%s\t%s\t%i\n", e,
								   AcousticMesh_Field( fieldA, name ), AcousticMesh_Field( fieldB, classname ), ( kind == 2 ) ? "in" : "out", kind,
								   AcousticReasonName( verdict.reason ), AcousticMesh_Field( fieldC, verdict.detail.c_str() ), verdict.numTris );
	}

	for( int m = 0; m < mesh.materials.Num(); m++ )
	{
		const acousticMaterial_t& material = mesh.materials[m];
		char source[32];
		switch( material.source )
		{
			case ACOUSTIC_SOURCE_SKY:
				idStr::Copynz( source, "sky", sizeof( source ) );
				break;
			case ACOUSTIC_SOURCE_LIQUID:
				idStr::Copynz( source, "liquid", sizeof( source ) );
				break;
			case ACOUSTIC_SOURCE_GLASS:
				idStr::Copynz( source, "glass", sizeof( source ) );
				break;
			case ACOUSTIC_SOURCE_SYNTHETIC:
				idStr::Copynz( source, "synthetic", sizeof( source ) );
				break;
			default:
				idStr::snPrintf( source, sizeof( source ), "type:%s", AcousticSurfaceTypeName( material.source ) );	// SEAMS D-3
				break;
		}
		const acousticNumbers_t& n = material.n;
		AcousticMesh_ManifestLine( &buffer, verbose, "material\t%s\t%s\t%.4f\t%.4f\t%.4f\t%.4f\t%.4f\t%.4f\t%.4f\n", AcousticMesh_Field( fieldA, material.name.c_str() ), source,
								   n.absorption[0], n.absorption[1], n.absorption[2], n.scattering, n.transmission[0], n.transmission[1], n.transmission[2] );
	}

	for( int d = 0; d < mesh.doors.Num(); d++ )
	{
		const acousticDoor_t& door = mesh.doors[d];
		const bool hasMaterial = ( door.material >= 0 && door.material < mesh.materials.Num() );
		AcousticMesh_ManifestLine( &buffer, verbose, "door\t%s\tportal\t%i\tareas\t%i\t%i\tplugTris\t%i\t%i\tmaterial\t%s\n",
								   AcousticMesh_Field( fieldA, door.name.c_str() ), door.portal, door.area0, door.area1, door.firstPlugTri, door.numPlugTris,
								   AcousticMesh_Field( fieldB, hasMaterial ? mesh.materials[door.material].name.c_str() : NULL ) );
	}

	const int numPlugTris = mesh.tris.Num() - mesh.numWorldTris;
	AcousticMesh_ManifestLine( &buffer, verbose, "totals\tverts\t%i\tworldTris\t%i\tplugTris\t%i\tin\t%i\tout\t%i\tkeyOut\t%i\twarnings\t%i\n",
							   mesh.verts.Num(), mesh.numWorldTris, numPlugTris, numIn, numOut, build.numKeyOut, build.numWarnings );

	if( !buffer.Close() )
	{
		common->Warning( "acoustics: error writing %s", path.c_str() );
		return false;
	}

	// R-SB1a-27; <plugs> is the plugged portals, the doors that own plug triangles (review M3-11); the warnings were
	// printed where they were found
	int numPlugs = 0;
	for( int d = 0; d < mesh.doors.Num(); d++ )
	{
		if( mesh.portals[mesh.doors[d].portal].door == d && mesh.doors[d].numPlugTris > 0 )
		{
			numPlugs++;
		}
	}
	common->Printf( "acoustics: mesh %i tris, %i plugs, %i in, %i out\n", mesh.numWorldTris, numPlugs, numIn, numOut );

	if( dmap_acousticDump.GetBool() )
	{
		AcousticMesh_WriteDump( build );
	}
	return true;
}
//puncho - SB1a-D end

// R-SB1a-24: materials by name, Icmp then Cmp
struct acousticMaterialOrder_t
{
	const char*		name;
	int				index;
};

// names are unique (AcousticMesh_AddMaterial), so the order is total and the quicksort's instability cannot show;
// negative puts a first, as idSort_QuickDefault
class idSort_AcousticMaterials : public idSort_Quick< acousticMaterialOrder_t, idSort_AcousticMaterials >
{
public:
	int Compare( const acousticMaterialOrder_t& a, const acousticMaterialOrder_t& b ) const
	{
		const int c = idStr::Icmp( a.name, b.name );
		return ( c != 0 ) ? c : idStr::Cmp( a.name, b.name );
	}
};

/*
============
AcousticStageA

R-SB1a-30, in this order: (a) settings, the reference hash, the script tokens; (b) every entity's verdict; (c) the
geometry of every entity that is in, then numWorldTris; (d) the portals; (e) the doors; (f) the material sort;
(g) the records and CRCs; (h) the invariants; (i) the acousticMesh time; (j) AcousticFinishStageA; (k) the outputs.
============
*/
bool AcousticStageA()
{
	const int startMs = Sys_Milliseconds();

	acousticBuild_t build;
	acousticMesh_t& mesh = build.mesh;
	const idMapFile* mapFile = dmapGlobals.dmapFile;
	const int numEntities = mapFile->GetNumEntities();

	mesh.mapFile = mapFile;
	mesh.worldTree = dmapGlobals.uEntities[0].tree;
	mesh.numAreas = dmapGlobals.uEntities[0].numAreas;
	mesh.numWorldTris = 0;
	mesh.entityKeysCRC = 0;
	mesh.materialHash = 0;
	mesh.scriptCRC = 0;
	mesh.materials.SetGranularity( 256 );	// verts and tris double: AcousticReserveOne
	mesh.brushPlanes.SetGranularity( 1024 );
	mesh.brushes.SetGranularity( 256 );
	mesh.statics.SetGranularity( 256 );		//puncho - SB1b-P; staticTris doubles: AcousticReserveOne
	build.numWarnings = 0;
	build.numKeyOut = 0;
	build.materialOverflow = false;
	build.weld.Clear( 16384, 16384 );
	build.weld.SetGranularity( 16384 );
	build.materialLookup.Clear( 1024, 1024 );

	if( numEntities != dmapGlobals.numEntities )
	{
		return AcousticMesh_Fail( startMs, va( "%i map entities for %i dmap entities", numEntities, dmapGlobals.numEntities ) );
	}
	if( mesh.worldTree == NULL )
	{
		return AcousticMesh_Fail( startMs, "no world tree" );
	}

	// (a) settings, the reference hash (S3a), the script tokens (S4)
	build.numWarnings += AcousticReadMeshSettings( mapFile->GetEntity( 0 )->epairs, mesh.settings );

	int numValues = 0;
	for( int e = 0; e < numEntities; e++ )
	{
		numValues += mapFile->GetEntity( e )->epairs.GetNumKeyVals();
	}
	build.references.SetNum( numValues );
	build.referenceHash.Clear( idMath::CeilPowerOfTwo( Max( numValues, 1024 ) ), Max( numValues, 1 ) );
	int numReferences = 0;
	for( int e = 0; e < numEntities; e++ )
	{
		const idDict& epairs = mapFile->GetEntity( e )->epairs;
		for( int k = 0; k < epairs.GetNumKeyVals(); k++ )
		{
			acousticReference_t& ref = build.references[numReferences];
			ref.entity = e;
			ref.value = &epairs.GetKeyVal( k )->GetValue();
			build.referenceHash.Add( AcousticMesh_MixKey( ( uint32 )idStr::IHash( ref.value->c_str() ) ), numReferences );
			numReferences++;
		}
	}

	acousticScriptSink_t sink;
	sink.file = AcousticTokenSink;
	sink.context = &build.scriptTokens;
	AcousticScriptWalk( mapFile->GetName(), sink );

	// (b) every entity's verdict, map order
	build.verdicts.SetNum( numEntities );
	for( int e = 0; e < numEntities; e++ )
	{
		AcousticMesh_Decide( build, e, build.verdicts[e] );
	}

	// (c) the geometry of every entity that is in, map order, world first
	idList<const uBrush_t*> byPrimitive;
	for( int e = 0; e < numEntities; e++ )
	{
		build.verdicts[e].numTris = AcousticMesh_EmitEntity( build, e, byPrimitive );
	}
	if( build.materialOverflow )
	{
		return AcousticMesh_Fail( startMs, "more than 65,535 materials" );
	}
	mesh.numWorldTris = mesh.tris.Num();

	// (d) the portals, interAreaPortals order; SB1a-D sets door
	mesh.portals.SetNum( interAreaPortals.Num() );
	for( int p = 0; p < interAreaPortals.Num(); p++ )
	{
		acousticPortal_t& portal = mesh.portals[p];
		portal.area0 = interAreaPortals[p].area0;
		portal.area1 = interAreaPortals[p].area1;
		portal.door = -1;
		AcousticMesh_PortalWinding( p )->GetPlane( portal.plane );
	}

	// (e) doors and plugs (SB1a-D)
	if( !AcousticMesh_Doors( build ) )
	{
		return AcousticMesh_Fail( startMs, "doors" );
	}
	if( build.materialOverflow )
	{
		return AcousticMesh_Fail( startMs, "more than 65,535 materials" );
	}

	// (f) the material sort; every triangle's material remapped after it, plugs included, and every door's
	{
		const int numMaterials = mesh.materials.Num();
		idList<acousticMaterialOrder_t> order;
		order.SetNum( numMaterials );
		for( int m = 0; m < numMaterials; m++ )
		{
			order[m].name = mesh.materials[m].name.c_str();
			order[m].index = m;
		}
		order.SortWithTemplate( idSort_AcousticMaterials() );

		idList<acousticMaterial_t> sorted;
		idList<int> remap;
		sorted.SetNum( numMaterials );
		remap.SetNum( numMaterials );
		for( int m = 0; m < numMaterials; m++ )
		{
			sorted[m] = mesh.materials[order[m].index];
			remap[order[m].index] = m;
		}
		mesh.materials = sorted;	// order[].name pointed into the old list: not read past here

		for( int t = 0; t < mesh.tris.Num(); t++ )
		{
			mesh.tris[t].material = ( uint16 )remap[mesh.tris[t].material];
		}
		for( int d = 0; d < mesh.doors.Num(); d++ )
		{
			if( mesh.doors[d].material >= 0 && mesh.doors[d].material < numMaterials )
			{
				mesh.doors[d].material = remap[mesh.doors[d].material];
			}
		}
		build.materialLookup.Free();	// indexes before the sort: no lookup after it
	}

	// (g) the records and CRCs: kinds first, then the kind 0 CRCs over the static candidates' names and the candidates' classes
	{
		idList<uint32> kinds;
		kinds.SetNum( numEntities );
		//puncho start - DR-148 (Y1): the static candidates' names and every candidate's class
		acousticNameSet_t staticNames;
		acousticNameSet_t candidateClasses;
		for( int e = 0; e < numEntities; e++ )
		{
			kinds[e] = ( uint32 )AcousticReasonKind( build.verdicts[e].reason );
			if( kinds[e] == 0 )
			{
				continue;
			}
			const idDict& epairs = mapFile->GetEntity( e )->epairs;
			const char* classname = epairs.GetString( "classname" );
			if( classname[0] != '\0' )
			{
				AcousticNameSet_Add( candidateClasses, classname, idStr::Length( classname ) );
			}
			const char* name = epairs.GetString( "name" );
			if( name[0] != '\0' && ( !idStr::Icmp( classname, "func_static" ) || !idStr::Icmp( classname, "misc_model" ) ) )
			{
				AcousticNameSet_Add( staticNames, name, idStr::Length( name ) );
			}
		}
		//puncho end

		mesh.entities.SetNum( numEntities );
		for( int e = 0; e < numEntities; e++ )
		{
			const idDict& epairs = mapFile->GetEntity( e )->epairs;
			acousticEntity_t& record = mesh.entities[e];
			record.name = epairs.GetString( "name" );
			record.entity = e;
			record.kind = kinds[e];
			record.crc = ( kinds[e] == 0 ) ? AcousticKind0CRC( epairs, staticNames, candidateClasses ) : mapFile->GetEntityCRC( e );	//puncho - DR-148 (Y1)
		}

		mesh.entityKeysCRC = AcousticEntityKeysCRC( mesh.entities );
		mesh.scriptCRC = AcousticScriptCRC( build.scriptTokens, *mapFile, kinds.Ptr(), numEntities );
		mesh.materialHash = AcousticMaterialHash( mesh.materials );
	}

	// (h) the invariants
	idStr cause;
	if( !AcousticMesh_Check( build, cause ) )
	{
		return AcousticMesh_Fail( startMs, cause.c_str() );
	}

	// (i) (a) to (h) under dmap_phaseTimes
	AcousticPhaseEnd( DP_ACOUSTIC_MESH, startMs );

	// (j) WP-SB1b: probes and .sndmesh
	if( !AcousticFinishStageA( mesh ) )
	{
		return AcousticMesh_FailNoOutputs( "probes or .sndmesh" );
	}

	// (k) the manifest, the console line, the dump (SB1a-D)
	if( !AcousticMesh_WriteOutputs( build ) )
	{
		// review M2R-2: the .sndmesh and .sndbake of this compile are valid; only the manifest, a diagnostic, goes
		common->Printf( "acoustics: Stage A failed: writing the manifest\n" );
		fileSystem->RemoveFile( va( "%s.sndmanifest", dmapGlobals.mapFileBase ) );
		return false;
	}

	return true;
}
