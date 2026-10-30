#include "../../idlib/precompiled.h"
#pragma hdrstop
#include "../Game_local.h"
#include "MapLandmarks.h"

const landmark_t* idMapLandmarks::FindLandmark( const char* name ) const
{
	for( int i = 0; i < landmarks.Num(); i++ )
	{
		if( landmarks[i].id.Icmp( name ) == 0 )
		{
			return &landmarks[i];
		}
	}
	return NULL;
}

const transitionVolume_t* idMapLandmarks::FindTransitionVolume( const char* name ) const
{
	for( int i = 0; i < transitionVolumes.Num(); i++ )
	{
		if( transitionVolumes[i].landmark.Icmp( name ) == 0 )
		{
			return &transitionVolumes[i];
		}
	}
	return NULL;
}

//puncho - P2b.1.1b: MapLoaded's town portal, re-added each load. RemoveIndex keeps the order of the rows left: a lookup
// returns the first match. The match is case-sensitive, unlike the finds, exactly as the inline loops it replaces were;
// a map-authored _TownPortal would survive it
void idMapLandmarks::RemoveById( const char* id )
{
	for( int i = landmarks.Num() - 1; i >= 0; i-- )
	{
		if( landmarks[i].id == id )
		{
			landmarks.RemoveIndex( i );
		}
	}
	for( int i = transitionVolumes.Num() - 1; i >= 0; i-- )
	{
		if( transitionVolumes[i].landmark == id )
		{
			transitionVolumes.RemoveIndex( i );
		}
	}
}

//puncho - P2b.1.10f: the group whose name equals the asked one by idStr::Icmp, as the per-entity scan matched it
const idStrList* idMapLandmarks::FindVariantChoices( const char* group ) const
{
	for( int i = 0; i < variantGroups.Num(); i++ )
	{
		if( variantGroups[i].group.Icmp( group ) == 0 )
		{
			return &variantGroups[i].choices;
		}
	}
	return NULL;
}

void idMapLandmarks::ReadMapData( const idMapFile* map )
{
	Clear();
	for( int i = 0; i < map->GetNumEntities(); i++ )
	{
		const idMapEntity* ent = map->GetEntity( i );
		const idDict& args = ent->epairs;
		//puncho start - P2b.1.10f: the dungeon_variant groups, read once here, before SpawnMapEntities, for InhibitSpawn,
		// which scanned every entity of the map for each variant entity. Every entity counts, the worldspawn too, as the
		// scan's did: a "group:choice" with a group (a colon past the first byte) adds its choice, an empty one too, to
		// the group that equals its group by Icmp, kept sorted and unique by Icmp
		const char* variant = args.GetString( "dungeon_variant" );
		const char* colon = strchr( variant, ':' );
		if( colon != NULL && colon != variant )
		{
			const idStr group( variant, 0, ( int )( colon - variant ) );
			int g = 0;
			while( g < variantGroups.Num() && variantGroups[g].group.Icmp( group ) != 0 )
			{
				g++;
			}
			if( g == variantGroups.Num() )
			{
				variantGroups.Alloc().group = group;
			}
			idStrList& choices = variantGroups[g].choices;
			const char* candidate = colon + 1;
			int at = 0;
			while( at < choices.Num() && choices[at].Icmp( candidate ) < 0 )
			{
				at++;
			}
			if( at == choices.Num() || choices[at].Icmp( candidate ) )
			{
				choices.Insert( candidate, at );
			}
		}
		//puncho end
		const bool isLandmark = idStr::Icmp( args.GetString( "classname" ), "info_landmark" ) == 0;
		const bool isVolume = idStr::Icmp( args.GetString( "classname" ), "trigger_transition" ) == 0;
		if( !isLandmark && !isVolume )
		{
			continue;
		}
		idBounds bounds;
		bounds.Clear();
		idVec3 center = args.GetVector( "origin" );
		//puncho - P2.4 (bug review 2026-10-28, stage 4b): the parsed brush planes are relative to the "origin" key in every
		// format (brushDef3 is written that way; brushDef and Valve 220 subtract the key on parse), so the key turns them
		// into world space. originOffset is an origin brush's pivot inside that same frame (CalculateBrushOrigin reads the
		// parsed planes), not a term of it: adding it misplaced a Valve 220 origin-brush landmark by its pivot, and leaving
		// the key out read every composed or hand-made brush landmark and carry volume with an origin key at local position
		const idVec3 entityOrigin = center;
		idVec3 forward = idAngles( 0, args.GetFloat( "angle" ), 0 ).ToForward();
		for( int p = 0; p < ent->GetNumPrimitives(); p++ )
		{
			if( ent->GetPrimitive( p )->GetType() != idMapPrimitive::TYPE_BRUSH )
			{
				continue;
			}
			const idMapBrush* brush = static_cast<const idMapBrush*>( ent->GetPrimitive( p ) );
			for( int side = 0; side < brush->GetNumSides(); side++ )
			{
				idWinding winding( brush->GetSide( side )->GetPlane() );
				for( int clip = 0; clip < brush->GetNumSides(); clip++ )
				{
					if( clip != side && !winding.ClipInPlace( -brush->GetSide( clip )->GetPlane(), 0.01f ) )
					{
						break;
					}
				}
				for( int v = 0; v < winding.GetNumPoints(); v++ )
				{
					bounds.AddPoint( winding[v].ToVec3() + entityOrigin );
				}
				if( isLandmark && idStr::Icmp( brush->GetSide( side )->GetMaterial(), "textures/editor/landmark" ) == 0 && winding.GetNumPoints() >= 3 )
				{
					double areaSum = 0, sum[3] = { 0, 0, 0 };
					const idVec3 a = winding[0].ToVec3();
					for( int v = 1; v + 1 < winding.GetNumPoints(); v++ )
					{
						const idVec3 b = winding[v].ToVec3(), c = winding[v + 1].ToVec3();
						double u[3], edgeV[3], cross[3];
						for( int axis = 0; axis < 3; axis++ )
						{
							u[axis] = ( double )b[axis] - a[axis];
							edgeV[axis] = ( double )c[axis] - a[axis];
						}
						cross[0] = u[1] * edgeV[2] - u[2] * edgeV[1];
						cross[1] = u[2] * edgeV[0] - u[0] * edgeV[2];
						cross[2] = u[0] * edgeV[1] - u[1] * edgeV[0];
						const double area = sqrt( cross[0] * cross[0] + cross[1] * cross[1] + cross[2] * cross[2] );
						areaSum += area;
						for( int axis = 0; axis < 3; axis++ )
						{
							sum[axis] += area * ( ( double )a[axis] + b[axis] + c[axis] ) / 3.0;
						}
					}
					if( areaSum > 0 )
					{
						for( int axis = 0; axis < 3; axis++ )
						{
							center[axis] = sum[axis] / areaSum + entityOrigin[axis];
						}
					}
					forward = brush->GetSide( side )->GetPlane().Normal();
				}
			}
		}
		//puncho - P2.4 (bug review 2026-10-28): an id over 63 bytes cannot enter a dungeon record or a fingerprint; such a
		// landmark or volume is skipped with a warning (an exit through it is refused as unknown) and mapGraphCheck
		// refuses it at author time (K12)
		const char* id = isLandmark ? args.GetString( "landmark", args.GetString( "name" ) ) : args.GetString( "landmark" );
		if( idStr::Length( id ) > 63 )
		{
			gameLocal.Warning( "%s: landmark id '%s' exceeds 63 bytes; skipped", map->GetName(), id );
			continue;
		}
		if( isLandmark )
		{
			landmark_t row;
			row.id = id;
			row.kind = args.GetString( "kind" );
			row.origin = center;
			row.axis[0] = forward;
			row.axis[2].Set( 0, 0, 1 );
			row.axis[1] = forward.Cross( row.axis[2] );
			row.flags = ( args.GetBool( "entry", "1" ) ? 1 : 0 ) | ( args.GetBool( "exit", "1" ) ? 2 : 0 ) | ( args.GetBool( "required" ) ? 4 : 0 );
			row.radius = args.GetFloat( "radius", "256" );
			row.link = args.GetString( "link" );
			row.sealedBy = args.GetString( "sealed_by" );
			row.ordinal = i;
			landmarks.Append( row );
		}
		else if( !bounds.IsCleared() )
		{
			transitionVolume_t row;
			row.bounds = bounds;
			row.landmark = id;
			row.cap = idMath::ClampInt( 0, 64, args.GetInt( "cap", "64" ) );
			row.carryAll = idStr::Icmp( args.GetString( "carry", "all" ), "all" ) == 0;
			row.ordinal = i;
			transitionVolumes.Append( row );
		}
	}
}
