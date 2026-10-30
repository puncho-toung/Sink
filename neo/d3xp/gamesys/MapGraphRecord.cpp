#include "../../idlib/precompiled.h"
#pragma hdrstop

#include "../Game_local.h"
#include "MapGraph.h"
#include "WorldStore.h"
#include "MapGraphLocal.h"

bool idMapGraphDirector::WriteRecord( idList<byte, TAG_GAME>& bytes ) const
{
	if( nodes.Num() > 256 || links.Num() > 1024 || pending.Num() > 1024 || flagDefinitions.Num() > 256 )
	{
		return false;
	}
	idGraphBytes b( bytes );
	b.Put( 2, 2 );
	b.Put( instanceId, 8 );
	b.Put( rootSeed, 8 );
	b.Text( graphName );
	b.Put( graphCRC, 4 );
	b.Put( 1, 2 );
	b.Put( createdWorldTime, 8 );
	b.Text( parentMap );
	b.Text( parentLandmark );
	b.Put( nodes.Num(), 2 );
	for( int i = 0; i < nodes.Num(); i++ )
	{
		const dungeonNode_t& n = nodes[i];
		b.Text( n.map );
		b.Put( n.kindIndex, 2 );
		b.Put( n.resetPolicy, 1 );
		b.Put( n.resetSeconds, 4 );
		b.Put( n.state, 1 );
		b.Put( n.visits, 2 );
		b.Put( n.depth, 2 );
		b.Put( n.geometryCRC, 4 );
		b.Put( n.landmarkCRC, 4 );
	}
	b.Put( links.Num(), 2 );
	for( int i = 0; i < links.Num(); i++ )
	{
		const dungeonLink_t& l = links[i];
		b.Put( l.from, 2 );
		b.Text( l.fromLandmark );
		b.Put( l.to, 2 );
		b.Text( l.toLandmark );
		b.Put( l.kind, 1 );
		b.Put( l.state, 1 );
		b.Put( l.condFlag < 0 ? 0 : l.condFlag, 1 );
		b.Put( l.conditionMode, 1 );
		b.Put( l.condValue, 2 );
	}
	int count = 0;
	for( int i = 0; i < 256; i++ )
	{
		if( flagValues[i] )
		{
			count++;
		}
	}
	b.Put( count, 2 );
	for( int i = 0; i < 256; i++ )
	{
		if( flagValues[i] )
		{
			b.Put( i, 1 );
			b.Put( flagValues[i], 2 );
		}
	}
	b.Put( partyNode, 2 );
	b.Text( partyLandmark );
	b.Put( portalNode, 2 );
	b.Text( portalLandmark );
	b.Put( pending.Num(), 2 );
	for( int i = 0; i < pending.Num(); i++ )
	{
		const dungeonPending_t& p = pending[i];
		b.Put( p.id, 4 );
		b.Put( p.targetNode, 2 );
		b.Text( p.targetMap );
		b.Text( p.targetName );
		b.Put( p.action, 1 );
		b.Put( p.queuedWorldTime, 8 );
	}
	b.Text( instanceEndKind );
	b.Put( flagDefinitions.Num(), 2 );
	for( int i = 0; i < flagDefinitions.Num(); i++ )
	{
		b.Text( flagDefinitions[i].name );
		b.Put( flagDefinitions[i].index, 1 );
	}
	b.Put( suspended ? 1 : 0, 1 );
	b.Put( portalOpen ? 1 : 0, 1 );
	for( int i = 0; i < 3; i++ )
	{
		uint32 bits;
		const float coordinate = portalOrigin[i];
		memcpy( &bits, &coordinate, 4 );
		b.Put( bits, 4 );
	}
	return b.Good();
}
bool idMapGraphDirector::ReadRecord( const byte* data, int size )
{
	if( size <= 0 || size > 512 * 1024 )
	{
		return false;
	}
	idMapGraphDirector temp;
	idGraphBytes b( data, size );
	if( b.Get( 2 ) != 2 )
	{
		return false;
	}
	temp.instanceId = b.Get( 8 );
	temp.rootSeed = b.Get( 8 );
	b.String( temp.graphName );
	temp.graphCRC = b.Get( 4 );
	if( b.Get( 2 ) != 1 )
	{
		return false;
	}
	temp.createdWorldTime = ( int64 )b.Get( 8 );
	b.String( temp.parentMap );
	b.String( temp.parentLandmark );
	int count = b.Get( 2 );
	if( count > 256 )
	{
		return false;
	}
	for( int i = 0; i < count; i++ )
	{
		dungeonNode_t n;
		b.String( n.map );
		n.kindIndex = b.Get( 2 );
		n.resetPolicy = b.Get( 1 );
		n.resetSeconds = b.Get( 4 );
		n.state = b.Get( 1 );
		n.visits = b.Get( 2 );
		n.depth = b.Get( 2 );
		n.geometryCRC = b.Get( 4 );
		n.landmarkCRC = b.Get( 4 );
		if( !b.ReadingOK() || n.map.IsEmpty() || n.kindIndex >= 256 || n.resetPolicy > 2 || n.state > 3 || n.depth > 256 )
		{
			return false;
		}
		temp.nodes.Append( n );
	}
	count = b.Get( 2 );
	if( count > 1024 )
	{
		return false;
	}
	for( int i = 0; i < count; i++ )
	{
		dungeonLink_t l;
		l.from = b.Get( 2 );
		b.String( l.fromLandmark );
		l.to = b.Get( 2 );
		b.String( l.toLandmark );
		l.kind = b.Get( 1 );
		l.state = b.Get( 1 );
		l.condFlag = b.Get( 1 );
		l.conditionMode = b.Get( 1 );
		l.condValue = b.Get( 2 );
		if( l.conditionMode == 0 )
		{
			l.condFlag = -1;
		}
		if( !b.ReadingOK() || l.from >= temp.nodes.Num() || ( l.to != 0xffff && l.to >= temp.nodes.Num() ) || l.state > 3 || l.kind > 1 || l.conditionMode > 2 || l.fromLandmark.IsEmpty() || ( l.to == 0xffff && l.state != 3 ) )
		{
			return false;
		}
		temp.links.Append( l );
	}
	count = b.Get( 2 );
	if( count > 256 )
	{
		return false;
	}
	int previous = -1;
	for( int i = 0; i < count; i++ )
	{
		const int index = b.Get( 1 );
		const uint16 value = b.Get( 2 );
		if( index <= previous || !value )
		{
			return false;
		}
		temp.flagValues[index] = value;
		previous = index;
	}
	temp.partyNode = b.Get( 2 );
	b.String( temp.partyLandmark );
	temp.portalNode = b.Get( 2 );
	b.String( temp.portalLandmark );
	if( ( temp.nodes.Num() && temp.partyNode >= temp.nodes.Num() ) || ( temp.portalNode != 0xffff && temp.portalNode >= temp.nodes.Num() ) )
	{
		return false;
	}
	count = b.Get( 2 );
	if( count > 1024 )
	{
		return false;
	}
	for( int i = 0; i < count; i++ )
	{
		dungeonPending_t p;
		p.id = b.Get( 4 );
		p.targetNode = b.Get( 2 );
		b.String( p.targetMap );
		b.String( p.targetName );
		p.action = b.Get( 1 );
		p.queuedWorldTime = ( int64 )b.Get( 8 );
		if( !b.ReadingOK() || p.action > 3 || p.targetMap.IsEmpty() || p.targetName.IsEmpty() || ( p.targetNode != 0xffff && p.targetNode >= temp.nodes.Num() ) )
		{
			return false;
		}
		for( int j = 0; j < temp.pending.Num(); j++ )
		{
			if( temp.pending[j].id == p.id )
			{
				return false;
			}
		}
		if( temp.pending.Num() )
		{
			const dungeonPending_t& prev = temp.pending[temp.pending.Num() - 1];
			if( p.queuedWorldTime < prev.queuedWorldTime || ( p.queuedWorldTime == prev.queuedWorldTime && p.id <= prev.id ) )
			{
				return false;
			}
		}
		temp.pending.Append( p );
	}
	b.String( temp.instanceEndKind );
	count = b.Get( 2 );
	if( count > 256 )
	{
		return false;
	}
	for( int i = 0; i < count; i++ )
	{
		graphFlag_t f;
		b.String( f.name );
		f.index = b.Get( 1 );
		if( f.name.IsEmpty() )
		{
			return false;
		}
		for( int j = 0; j < temp.flagDefinitions.Num(); j++ )
		{
			if( f.index == temp.flagDefinitions[j].index || !f.name.Icmp( temp.flagDefinitions[j].name ) )
			{
				return false;
			}
		}
		temp.flagDefinitions.Append( f );
	}
	const int suspendState = b.Get( 1 );
	const int portalState = b.Get( 1 );
	if( suspendState > 1 || portalState > 1 )
	{
		return false;
	}
	temp.suspended = suspendState != 0;
	temp.portalOpen = portalState != 0;
	for( int i = 0; i < 3; i++ )
	{
		const uint32 bits = b.Get( 4 );
		memcpy( &temp.portalOrigin[i], &bits, 4 );
		if( ( bits & 0x7f800000U ) == 0x7f800000U )
		{
			return false;
		}
	}
	if( !b.Good() )
	{
		return false;
	}
	*this = temp;
	return true;
}
void idMapGraphDirector::StoreRecord()
{
	if( common->IsClient() || !worldStore.IsActive() )
	{
		return;
	}
	//puncho - P2.4 (bug review 2026-10-28): an overlong name keeps the previous record with a warning instead of an
	// Error at the leave or the save; the parser, ReadMapData and QueueEffect refuse names over 63 bytes
	idList<byte, TAG_GAME> record;
	if( !WriteRecord( record ) )
	{
		gameLocal.Warning( "dungeon record not stored: a name exceeds 63 bytes; the previous record is kept" );
		return;
	}
	worldStore.SetDungeonRecords( persistentHub, record );
}
bool idMapGraphDirector::RestoreRecord()
{
	Clear();
	if( worldStore.NumDungeonRecords() == 0 )
	{
		return true;
	}
	if( worldStore.NumDungeonRecords() > 2 )
	{
		return false;
	}
	for( int i = 0; i < worldStore.NumDungeonRecords(); i++ )
	{
		const idList<byte, TAG_GAME>& bytes = worldStore.DungeonRecord( i );
		idMapGraphDirector record;
		if( !record.ReadRecord( bytes.Ptr(), bytes.Num() ) )
		{
			Clear();
			return false;
		}
		if( i == 0 && worldStore.NumDungeonRecords() == 2 )
		{
			if( record.instanceId != 0 )
			{
				return false;
			}
			persistentHub = bytes;
		}
		else
		{
			idList<byte, TAG_GAME> hub = persistentHub;
			*this = record;
			persistentHub = hub;
		}
	}
	recipeValid = false;
	const bool continuation = worldStore.GetContinue();
	worldStore.SetContinue( true );
	RecipeForLoad( gameLocal.GetMapName() );
	worldStore.SetContinue( continuation );
	return true;
}
