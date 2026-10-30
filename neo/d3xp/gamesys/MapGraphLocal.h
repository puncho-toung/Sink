#ifndef __GAME_MAPGRAPHLOCAL_H__
#define __GAME_MAPGRAPHLOCAL_H__

//puncho - P2b.1.1a: what MapGraph.cpp (parser, generator), MapGraphRecord.cpp (the dungeon record), MapGraphDirector.cpp
// (the director) and MapGraphCmds.cpp (its console commands) share

idStr GraphMapName( const char* name );
idStr GraphEffectMapName( const char* name );
bool GraphEffectMatches( const idStr& target, const idStr& graphMap );
const idDeclMapGraph* FindGraph( const char* name );

void MapGraphStart_f( const idCmdArgs& args );
void MapGraphDump_f( const idCmdArgs& args );
void ListDungeon_f( const idCmdArgs& args );
void ListMapGraphs_f( const idCmdArgs& args );
void SetDungeonFlag_f( const idCmdArgs& args );
#if !defined( ID_RETAIL )
void MapGraphTest_f( const idCmdArgs& args );
void MapGraphPortalTest_f( const idCmdArgs& args );
#endif

// Fixed-width little-endian records. Every read checks the remaining bytes before
// advancing; restore parses into a temporary director and publishes only on success.
class idGraphBytes
{
public:
	idGraphBytes( idList<byte, TAG_GAME>& dest ) : output( &dest ), data( NULL ), size( 0 ), pos( 0 ), good( true )
	{
		dest.Clear();
	}
	idGraphBytes( const byte* bytes, int count ) : output( NULL ), data( bytes ), size( count ), pos( 0 ), good( count >= 0 && ( bytes != NULL || count == 0 ) )
	{
	}
	void Number( uint64& value, int count )
	{
		if( output )
		{
			Reserve( count );
			for( int i = 0; i < count; i++ )
			{
				output->Append( ( byte )( value >> ( i * 8 ) ) );
			}
		}
		else
		{
			value = 0;
			if( !good || count > size - pos )
			{
				good = false;
				return;
			}
			for( int i = 0; i < count; i++ )
			{
				value |= ( uint64 )data[pos++] << ( i * 8 );
			}
		}
	}
	uint64 Get( int count )
	{
		uint64 n = 0;
		Number( n, count );
		return n;
	}
	void Put( uint64 n, int count )
	{
		Number( n, count );
	}
	void String( idStr& str, int max = 63 )
	{
		if( output )
		{
			if( str.Length() > max )
			{
				good = false;
				return;
			}
			Put( str.Length(), 1 );
			Reserve( str.Length() );
			for( int i = 0; i < str.Length(); i++ )
			{
				output->Append( ( byte )str[i] );
			}
		}
		else
		{
			const int len = Get( 1 );
			str.Clear();
			if( !good || len > max || len > size - pos )
			{
				good = false;
				return;
			}
			for( int i = 0; i < len; i++ )
			{
				if( data[pos] == 0 )
				{
					good = false;
				}
				str.Append( ( char )data[pos++] );
			}
		}
	}
	void Text( const idStr& str, int max = 63 )
	{
		idStr copy = str;
		String( copy, max );
	}
	bool Good() const
	{
		return good && ( output || pos == size );
	}
	bool ReadingOK() const
	{
		return good;
	}

private:
	//puncho - P2.4 (bug review 2026-10-28): idList grows by its granularity (16), so a byte-by-byte record of a few
	// hundred KiB reallocated and copied thousands of times; the capacity doubles instead
	void Reserve( int count )
	{
		if( output->Num() + count > output->NumAllocated() )
		{
			output->Resize( Max( 256, ( output->Num() + count ) * 2 ) );
		}
	}
	idList<byte, TAG_GAME>* output;
	const byte* data;
	int size, pos;
	bool good;
};

extern idCVar g_dungeonSeed;
#endif
