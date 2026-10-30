#ifndef __CELLID_H__
#define __CELLID_H__

/*
===============================================================================

	C1 cell ids: 8192-unit cubes in the placeable [-15, 15] grid on every axis.
	CELL_ID_NONE (0) and CELL_ID_PERSISTENT (1) occupy the reserved cz = -128
	layer and are never placeable. uint32 and NULL are supplied before inclusion.

===============================================================================
*/

#define CELL_SIZE			8192
#define CELL_HALF_SIZE		4096
#define CELL_GRID_MAX		15
#define CELL_CODE_SIZE		12
#define CELL_ID_NONE		0u
#define CELL_ID_PERSISTENT	1u

// The caller supplies cx/cy in [-2048, 2047] and cz in [-128, 127].
inline uint32 CellId( int cx, int cy, int cz )
{
	return ( (uint32)( cz + 128 ) << 24 ) | ( (uint32)( cy + 2048 ) << 12 ) | (uint32)( cx + 2048 );
}

inline int CellX( uint32 id )
{
	return (int)( id & 0xFFFu ) - 2048;
}

inline int CellY( uint32 id )
{
	return (int)( ( id >> 12 ) & 0xFFFu ) - 2048;
}

inline int CellZ( uint32 id )
{
	return (int)( id >> 24 ) - 128;
}

inline bool CellPlaceable( uint32 id )
{
	const int cx = CellX( id );
	const int cy = CellY( id );
	const int cz = CellZ( id );
	return cx >= -CELL_GRID_MAX && cx <= CELL_GRID_MAX
		&& cy >= -CELL_GRID_MAX && cy <= CELL_GRID_MAX
		&& cz >= -CELL_GRID_MAX && cz <= CELL_GRID_MAX;
}

inline void CellCode( uint32 id, char out[CELL_CODE_SIZE] )
{
	out[0] = 'c';
	// CELL_ID_PERSISTENT lies on the reserved layer, so it is never placeable
	if ( !CellPlaceable( id ) )
	{
		out[1] = ( id == CELL_ID_PERSISTENT ) ? 'P' : '?';
		out[2] = '\0';
		return;
	}

	const int coordinates[3] = { CellX( id ), CellY( id ), CellZ( id ) };
	int length = 1;
	for ( int axis = 0; axis < 3; ++axis )
	{
		int value = coordinates[axis];
		if ( axis == 2 && value == 0 )
		{
			break;
		}
		out[length++] = ( value < 0 ) ? "WSD"[axis] : "ENU"[axis];
		if ( value < 0 )
		{
			value = -value;
		}
		if ( value >= 10 )
		{
			out[length++] = (char)( '0' + value / 10 );
		}
		out[length++] = (char)( '0' + value % 10 );
	}
	out[length] = '\0';
}

inline bool ParseCellCode( const char* str, uint32& id )
{
	if ( str == NULL || str[0] != 'c' )
	{
		return false;
	}
	const char* p = str + 1;
	if ( p[0] == 'P' && p[1] == '\0' )
	{
		id = CELL_ID_PERSISTENT;
		return true;
	}

	int coordinates[3] = { 0, 0, 0 };
	for ( int axis = 0; axis < 3; ++axis )
	{
		if ( axis == 2 && *p == '\0' )
		{
			break;
		}
		const bool negative = ( *p == "WSD"[axis] );
		if ( !negative && *p != "ENU"[axis] )
		{
			return false;
		}
		++p;
		if ( *p < '0' || *p > '9' )
		{
			return false;
		}
		int value = *p++ - '0';
		if ( value == 0 )
		{
			if ( negative || axis == 2 )
			{
				return false;
			}
		}
		else if ( *p >= '0' && *p <= '9' )
		{
			value = value * 10 + ( *p++ - '0' );
		}
		if ( value > CELL_GRID_MAX )
		{
			return false;
		}
		coordinates[axis] = negative ? -value : value;
	}
	if ( *p != '\0' )
	{
		return false;
	}
	// Commit only after the entire canonical spelling has been accepted.
	id = CellId( coordinates[0], coordinates[1], coordinates[2] );
	return true;
}

#endif // __CELLID_H__
