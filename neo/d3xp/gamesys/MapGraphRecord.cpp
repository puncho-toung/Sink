#include "../../idlib/precompiled.h"
#pragma hdrstop

#include "../Game_local.h"
#include "MapGraph.h"
#include "WorldStore.h"
#include "MapGraphLocal.h"

//puncho start - P2b.1.4: the dungeon record (DREC v2) through MapGraph.h's struct lists and Record.h's codec. The
// reader refuses what the hand reader refused, in its order: each list's count before any element, then per element
// its fix-up and checks (the Check functions), the literals, the sparse flag list's order and zeros, the bools past 1,
// a non-finite portal origin, a short record and a byte past its end. One test moved: the party and portal nodes are
// tested after the whole record (the hand reader tested them before the pending list), so the same records are refused
static void WriteRecordCondFlag( idRecordOut& rec, const dungeonLink_t& l )
{
	rec.WriteUnsigned( l.condFlag < 0 ? 0 : l.condFlag, 1 );
}
static bool ReadRecordCondFlag( idRecordIn& rec, dungeonLink_t& l )
{
	byte flag;
	if( !rec.ReadByte( flag ) )
	{
		return false;
	}
	l.condFlag = flag;
	return true;
}

SCHEMA_RECORD_STRUCT_FUNCTIONS( Node, dungeonNode_t, DUNGEON_NODE_SCHEMA )
SCHEMA_RECORD_STRUCT_FUNCTIONS( Link, dungeonLink_t, DUNGEON_LINK_SCHEMA )
SCHEMA_RECORD_STRUCT_FUNCTIONS( Pending, dungeonPending_t, DUNGEON_PENDING_SCHEMA )
SCHEMA_RECORD_STRUCT_FUNCTIONS( FlagDef, graphFlag_t, DUNGEON_FLAGDEF_SCHEMA )

static bool CheckNode( dungeonNode_t& n, const idMapGraphDirector& owner )
{
	return !n.map.IsEmpty() && n.kindIndex < 256 && n.resetPolicy <= 2 && n.state <= 3 && n.depth <= 256;
}
static bool CheckLink( dungeonLink_t& l, const idMapGraphDirector& owner )
{
	if( l.conditionMode == 0 )
	{
		l.condFlag = -1;
	}
	return !( l.from >= owner.nodes.Num() || ( l.to != 0xffff && l.to >= owner.nodes.Num() ) || l.state > 3 || l.kind > 1 || l.conditionMode > 2 || l.fromLandmark.IsEmpty() || ( l.to == 0xffff && l.state != 3 ) );
}
static bool CheckPending( dungeonPending_t& p, const idMapGraphDirector& owner )
{
	if( p.action > 3 || p.targetMap.IsEmpty() || p.targetName.IsEmpty() || ( p.targetNode != 0xffff && p.targetNode >= owner.nodes.Num() ) )
	{
		return false;
	}
	for( int j = 0; j < owner.pending.Num(); j++ )
	{
		if( owner.pending[j].id == p.id )
		{
			return false;
		}
	}
	if( owner.pending.Num() )
	{
		const dungeonPending_t& prev = owner.pending[owner.pending.Num() - 1];
		if( p.queuedWorldTime < prev.queuedWorldTime || ( p.queuedWorldTime == prev.queuedWorldTime && p.id <= prev.id ) )
		{
			return false;
		}
	}
	return true;
}
static bool CheckFlagDef( graphFlag_t& f, const idMapGraphDirector& owner )
{
	if( f.name.IsEmpty() )
	{
		return false;
	}
	for( int j = 0; j < owner.flagDefinitions.Num(); j++ )
	{
		if( f.index == owner.flagDefinitions[j].index || !f.name.Icmp( owner.flagDefinitions[j].name ) )
		{
			return false;
		}
	}
	return true;
}

static void WriteRecordVersion( idRecordOut& rec, const idMapGraphDirector& s )
{
	rec.WriteUnsigned( DUNGEON_RECORD_VERSION, 2 );
}
static bool ReadRecordVersion( idRecordIn& rec, idMapGraphDirector& s )
{
	uint16 version;
	return rec.ReadUnsignedShort( version ) && version == DUNGEON_RECORD_VERSION;
}
static void WriteRecordGeneratorVersion( idRecordOut& rec, const idMapGraphDirector& s )
{
	rec.WriteUnsigned( DUNGEON_GENERATOR_VERSION, 2 );
}
static bool ReadRecordGeneratorVersion( idRecordIn& rec, idMapGraphDirector& s )
{
	uint16 version;
	return rec.ReadUnsignedShort( version ) && version == DUNGEON_GENERATOR_VERSION;
}
// the set flags only, ascending: a uint16 count, then ( uint8 index, uint16 value ) each; the reader refuses an index
// out of order and a zero value
static void WriteRecordFlagValues( idRecordOut& rec, const idMapGraphDirector& s )
{
	int count = 0;
	for( int i = 0; i < 256; i++ )
	{
		if( s.flagValues[i] )
		{
			count++;
		}
	}
	rec.WriteUnsigned( count, 2 );
	for( int i = 0; i < 256; i++ )
	{
		if( s.flagValues[i] )
		{
			rec.WriteUnsigned( i, 1 );
			rec.WriteUnsigned( s.flagValues[i], 2 );
		}
	}
}
static bool ReadRecordFlagValues( idRecordIn& rec, idMapGraphDirector& s )
{
	int count;
	if( !rec.ReadCount( count, 2, 256 ) )
	{
		return false;
	}
	int previous = -1;
	for( int i = 0; i < count; i++ )
	{
		byte index;
		uint16 value;
		if( !rec.ReadByte( index ) || !rec.ReadUnsignedShort( value ) || index <= previous || !value )
		{
			return false;
		}
		s.flagValues[index] = value;
		previous = index;
	}
	return true;
}
static void WriteRecordSuspended( idRecordOut& rec, const idMapGraphDirector& s )
{
	rec.WriteUnsigned( s.suspended ? 1 : 0, 1 );
}
static bool ReadRecordSuspended( idRecordIn& rec, idMapGraphDirector& s )
{
	byte state;
	if( !rec.ReadByte( state ) || state > 1 )
	{
		return false;
	}
	s.suspended = state != 0;
	return true;
}
static void WriteRecordPortalOpen( idRecordOut& rec, const idMapGraphDirector& s )
{
	rec.WriteUnsigned( s.portalOpen ? 1 : 0, 1 );
}
static bool ReadRecordPortalOpen( idRecordIn& rec, idMapGraphDirector& s )
{
	byte state;
	if( !rec.ReadByte( state ) || state > 1 )
	{
		return false;
	}
	s.portalOpen = state != 0;
	return true;
}

SCHEMA_RECORD_STRUCT_FUNCTIONS( DungeonRecord, idMapGraphDirector, DUNGEON_RECORD_SCHEMA )

// bytes is left as it was when the record cannot be written (a name past 63 bytes, a count past its list's bound)
bool idMapGraphDirector::WriteRecord( idList<byte, TAG_GAME>& bytes ) const
{
	// sized once for the record bytes held (a rewrite of the hub's) or 4 KiB; a record past that grows in 16 KiB steps,
	// per store, never per tic
	idFile_Memory file( "DREC" );
	file.PreAllocate( Max( bytes.Num(), 4096 ) + 1 );
	idRecordOut rec;
	rec.BeginFile( &file );
	WriteRecordDungeonRecord( rec, *this );
	if( rec.Overflowed() )
	{
		return false;
	}
	bytes.SetNum( file.Length() );
	memcpy( bytes.Ptr(), file.GetDataPtr(), file.Length() );
	return true;
}
// restore parses into a temporary director and publishes only on success
bool idMapGraphDirector::ReadRecord( const byte* data, int size )
{
	if( data == NULL || size <= 0 || size > 512 * 1024 )
	{
		return false;
	}
	idMapGraphDirector temp;
	idFile_Memory file( "DREC", ( const char* )data, size );
	idRecordIn rec;
	rec.BeginFile( &file );
	ReadRecordDungeonRecord( rec, temp );
	if( !rec.good || file.Tell() != file.Length() || ( temp.nodes.Num() && temp.partyNode >= temp.nodes.Num() ) || ( temp.portalNode != 0xffff && temp.portalNode >= temp.nodes.Num() ) )
	{
		return false;
	}
	*this = temp;
	return true;
}
//puncho end
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
