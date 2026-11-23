/*
===========================================================================
R5 speed (puncho): candidates for the reachability pair loop.

idAASReach::Build asks, for every area i, every area j whether a step, a barrier jump, a water jump or a walk
off a ledge leads from i to j. Reachability_Step_Barrier_WaterJump_WalkOffLedge says no to nearly every pair at
its bounds test ( the x-y bounds of the two areas lie more than 2 units apart ), and up to that test neither it
nor the loop round it writes anything. So the loop may leave out any j that fails the test.

A CANDIDATE LIST of area i holds every area j that has AREA_REACHABLE_WALK and passes the bounds test against
i. It may hold more areas. It is ascending and holds no area twice. The loop then runs its old body for the
candidates, and the body runs the bounds test again.

  AAS_ReachCandidates       the bounds test for every j, on a packed copy of the bounds, four areas at a time
  AAS_ReachCheckArea        the bounds test as it was, one pair at a time, for every j, against a candidate
                            list ( aas_reachCheck, on by default )
===========================================================================
*/

#ifndef __AASREACH_FAST_H__
#define __AASREACH_FAST_H__

// The packed test below is the pair function's own bounds test only where float arithmetic is done in float and as
// it is written: AAS_ReachPackBuild stores bounds + 2.0f rounded to float, and the pair function compares against
// the sum as the compiler evaluates it. With x87 arithmetic ( a 32-bit build without SSE2 ) that sum is kept in
// extended precision, and a fast floating point model may rewrite the comparison: there a pair on the border could
// be left out here that the pair function would take.
// Refused when this file is compiled: FLT_EVAL_METHOD other than 0; -ffast-math of GCC and Clang ( __FAST_MATH__:
// their FLT_EVAL_METHOD stays 0 under it ); /fp:fast of MSVC ( _M_FP_FAST ).
// NOT seen here: single options of the fast model given without -ffast-math ( -funsafe-math-optimizations,
// -fassociative-math and the like ). This tree's build files set none ( neo/CMakeLists.txt ).
// In every build the check of aas_reachCheck, on by default, runs the pair function's test for every pair beside
// the packed one, and the map compile ends with an error when a pair that passes it is not among the candidates.
#include <float.h>
#if ( defined( FLT_EVAL_METHOD ) && FLT_EVAL_METHOD != 0 ) || defined( __FAST_MATH__ ) || defined( _M_FP_FAST )
	#error "AASReach_fast.h: this build does not do float arithmetic in float as it is written ( FLT_EVAL_METHOD is not 0, or a fast floating point model ): the packed bounds test could differ from the pair function's"
#endif

/*
================
AAS_ReachBoundsPass

  the bounds test of Reachability_Step_Barrier_WaterJump_WalkOffLedge, word for word
================
*/
ID_INLINE bool AAS_ReachBoundsPass( const aasArea_t* area1, const aasArea_t* area2 )
{
	int i;

	// if the areas are not near anough in the x-y direction
	for( i = 0; i < 2; i++ )
	{
		if( area1->bounds[0][i] > area2->bounds[1][i] + 2.0f )
		{
			return false;
		}
		if( area1->bounds[1][i] < area2->bounds[0][i] - 2.0f )
		{
			return false;
		}
	}
	return true;
}

/*
================
aasReachPack_t

  what the bounds test reads of its second area, for every area: the right-hand sides of the four comparisons
================
*/
typedef struct aasReachPack_s
{
	int					numAreas;
	int					numPadded;			// numAreas rounded up to a multiple of four
	float* 				block;				// the four arrays, one allocation
	float* 				maxXp;				// bounds[1][0] + 2.0f
	float* 				minXm;				// bounds[0][0] - 2.0f
	float* 				maxYp;				// bounds[1][1] + 2.0f
	float* 				minYm;				// bounds[0][1] - 2.0f
} aasReachPack_t;

// false when there is no memory for it ( Mem_Alloc16 returns NULL then ): the pack is empty and may be freed
ID_INLINE bool AAS_ReachPackBuild( aasReachPack_t& pack, const aasArea_t* areas, const int numAreas )
{
	int j;

	pack.numAreas = numAreas;
	pack.numPadded = ( numAreas + 3 ) & ~3;
	pack.block = ( float* )Mem_Alloc16( ( size_t )pack.numPadded * 4 * sizeof( float ), TAG_AAS );
	if( pack.block == NULL )
	{
		pack.numAreas = pack.numPadded = 0;
		pack.maxXp = pack.minXm = pack.maxYp = pack.minYm = NULL;
		return false;
	}
	pack.maxXp = pack.block;
	pack.minXm = pack.block + pack.numPadded;
	pack.maxYp = pack.block + pack.numPadded * 2;
	pack.minYm = pack.block + pack.numPadded * 3;

	for( j = 0; j < numAreas; j++ )
	{
		// in single precision, as the test computes them
		pack.maxXp[j] = areas[j].bounds[1][0] + 2.0f;
		pack.minXm[j] = areas[j].bounds[0][0] - 2.0f;
		pack.maxYp[j] = areas[j].bounds[1][1] + 2.0f;
		pack.minYm[j] = areas[j].bounds[0][1] - 2.0f;
	}
	for( ; j < pack.numPadded; j++ )
	{
		// the padding fails the test; AAS_ReachCandidates leaves it out by its number in any case
		pack.maxXp[j] = pack.maxYp[j] = -idMath::INFINITUM;
		pack.minXm[j] = pack.minYm[j] = idMath::INFINITUM;
	}
	return true;
}

ID_INLINE void AAS_ReachPackFree( aasReachPack_t& pack )
{
	Mem_Free16( pack.block );
	pack.block = pack.maxXp = pack.minXm = pack.maxYp = pack.minYm = NULL;
}

/*
================
AAS_ReachCandidates

  Every area j, ascending, that passes the bounds test against area: exactly those ( with or without
  AREA_REACHABLE_WALK ). out must have room for pack.numAreas numbers. Returns how many were written.

  The test fails when  area min > j max + 2  or  area max < j min - 2 , on x or on y. A comparison with a
  value that is not a number is false in the test's > and < and in the packed comparisons alike, so such a
  pair passes in both.
================
*/
ID_INLINE int AAS_ReachCandidates( const aasReachPack_t& pack, const aasArea_t* area, int* out )
{
	int j, num = 0;
	const float aMinX = area->bounds[0][0], aMaxX = area->bounds[1][0];
	const float aMinY = area->bounds[0][1], aMaxY = area->bounds[1][1];

#if defined( USE_INTRINSICS_SSE )
	int b, keep;
	__m128 fail;
	const __m128 vMinX = _mm_set1_ps( aMinX ), vMaxX = _mm_set1_ps( aMaxX );
	const __m128 vMinY = _mm_set1_ps( aMinY ), vMaxY = _mm_set1_ps( aMaxY );

	for( j = 0; j < pack.numPadded; j += 4 )
	{
		fail = _mm_or_ps( _mm_cmpgt_ps( vMinX, _mm_load_ps( pack.maxXp + j ) ), _mm_cmplt_ps( vMaxX, _mm_load_ps( pack.minXm + j ) ) );
		keep = ~_mm_movemask_ps( fail ) & 15;
		if( keep == 0 )
		{
			continue;
		}
		fail = _mm_or_ps( _mm_cmpgt_ps( vMinY, _mm_load_ps( pack.maxYp + j ) ), _mm_cmplt_ps( vMaxY, _mm_load_ps( pack.minYm + j ) ) );
		keep &= ~_mm_movemask_ps( fail );
		for( b = 0; keep != 0; b++, keep >>= 1 )
		{
			if( ( keep & 1 ) != 0 && j + b < pack.numAreas )
			{
				out[num++] = j + b;
			}
		}
	}
#else
	for( j = 0; j < pack.numAreas; j++ )
	{
		if( aMinX > pack.maxXp[j] || aMaxX < pack.minXm[j] || aMinY > pack.maxYp[j] || aMaxY < pack.minYm[j] )
		{
			continue;
		}
		out[num++] = j;
	}
#endif
	return num;
}

/*
================
aasReachRaw_t

  What the check of aas_reachCheck reads of the second area of a pair, for every area: the four x-y bounds as
  they are in the area, nothing added, and whether the area has AREA_REACHABLE_WALK. 17 bytes an area where the
  area's own record has 80: at 330,000 areas the check of one area reads 5.6 MB, not 26 MB, and the check is
  run for every area. AAS_ReachRawSame says whether the copy is still the areas' own numbers.
================
*/
typedef struct aasReachRaw_s
{
	int					numAreas;
	float* 				block;				// the four arrays and the flags, one allocation
	float* 				minX;				// bounds[0][0]
	float* 				maxX;				// bounds[1][0]
	float* 				minY;				// bounds[0][1]
	float* 				maxY;				// bounds[1][1]
	byte* 				walk;				// 1 = the area has AREA_REACHABLE_WALK, else 0
} aasReachRaw_t;

// false when there is no memory for it ( Mem_Alloc16 returns NULL then ): the copy is empty and may be freed
ID_INLINE bool AAS_ReachRawBuild( aasReachRaw_t& raw, const aasArea_t* areas, const int numAreas )
{
	int j;
	const size_t num = ( size_t )numAreas;

	raw.numAreas = numAreas;
	raw.block = ( float* )Mem_Alloc16( num * 4 * sizeof( float ) + num, TAG_AAS );
	if( raw.block == NULL )
	{
		raw.numAreas = 0;
		raw.minX = raw.maxX = raw.minY = raw.maxY = NULL;
		raw.walk = NULL;
		return false;
	}
	raw.minX = raw.block;
	raw.maxX = raw.block + num;
	raw.minY = raw.block + num * 2;
	raw.maxY = raw.block + num * 3;
	raw.walk = ( byte* )( raw.block + num * 4 );

	for( j = 0; j < numAreas; j++ )
	{
		raw.minX[j] = areas[j].bounds[0][0];
		raw.maxX[j] = areas[j].bounds[1][0];
		raw.minY[j] = areas[j].bounds[0][1];
		raw.maxY[j] = areas[j].bounds[1][1];
		raw.walk[j] = ( areas[j].flags & AREA_REACHABLE_WALK ) ? 1 : 0;
	}
	return true;
}

ID_INLINE void AAS_ReachRawFree( aasReachRaw_t& raw )
{
	Mem_Free16( raw.block );
	raw.block = raw.minX = raw.maxX = raw.minY = raw.maxY = NULL;
	raw.walk = NULL;
}

/*
================
AAS_ReachRawSame

  -1 when the copy holds, bit for bit, the four bounds and the flag of every area as the areas have them now.
  Otherwise the number of the first area that differs. Asked after the pair loop: then the check compared the
  numbers the pair function read, and no area's bounds or flag changed while the loop ran.
================
*/
ID_INLINE int AAS_ReachRawSame( const aasReachRaw_t& raw, const aasArea_t* areas, const int numAreas )
{
	int j;

	if( raw.numAreas != numAreas || raw.block == NULL )
	{
		return 0;
	}
	for( j = 0; j < numAreas; j++ )
	{
		const float inArea[4] = { areas[j].bounds[0][0], areas[j].bounds[1][0], areas[j].bounds[0][1], areas[j].bounds[1][1] };
		const float inCopy[4] = { raw.minX[j], raw.maxX[j], raw.minY[j], raw.maxY[j] };
		const byte walk = ( areas[j].flags & AREA_REACHABLE_WALK ) ? 1 : 0;

		// the bits, not the values: a bound that is not a number equals itself here
		if( memcmp( inArea, inCopy, sizeof( inArea ) ) != 0 || raw.walk[j] != walk )
		{
			return j;
		}
	}
	return -1;
}

/*
================
AAS_ReachCheckArea

  The check of aas_reachCheck, on by default. -1: candidates is ascending without repeats, inside [0, numAreas),
  and holds every area with AREA_REACHABLE_WALK that passes the old bounds test against area. Otherwise the
  number of the first such area that is missing, or -2 when the list is out of order or out of range.

  The test is the one of AAS_ReachBoundsPass above, written out for the two axes: the same four comparisons on
  the same numbers in the same order, one pair at a time, for EVERY area j. Nothing here comes from the packed
  copy, whose numbers have the 2.0f added already, and nothing is left out by position. What it reads of area j
  it reads from raw, so that it does not pull every area's record through the cache for every area.
================
*/
ID_INLINE int AAS_ReachCheckArea( const aasReachRaw_t& raw, const aasArea_t* area, const int* candidates, const int numCandidates )
{
	int j, k;
	const int numAreas = raw.numAreas;
	const float aMinX = area->bounds[0][0], aMaxX = area->bounds[1][0];
	const float aMinY = area->bounds[0][1], aMaxY = area->bounds[1][1];

	for( k = 0; k < numCandidates; k++ )
	{
		if( candidates[k] < 0 || candidates[k] >= numAreas || ( k > 0 && candidates[k] <= candidates[k - 1] ) )
		{
			return -2;
		}
	}
	k = 0;
	for( j = 0; j < numAreas; j++ )
	{
		if( !raw.walk[j] )
		{
			continue;
		}
		// if the areas are not near anough in the x-y direction
		if( aMinX > raw.maxX[j] + 2.0f )
		{
			continue;
		}
		if( aMaxX < raw.minX[j] - 2.0f )
		{
			continue;
		}
		if( aMinY > raw.maxY[j] + 2.0f )
		{
			continue;
		}
		if( aMaxY < raw.minY[j] - 2.0f )
		{
			continue;
		}
		// j passes: it must be the next candidate that is not below it
		while( k < numCandidates && candidates[k] < j )
		{
			k++;
		}
		if( k >= numCandidates || candidates[k] != j )
		{
			return j;
		}
	}
	return -1;
}

/*
================
AAS_ReachFaultDrop

  The planted fault of aas_fault 1, a test of the check above: takes out of the list the last candidate the
  check asks for ( an area with AREA_REACHABLE_WALK that passes the bounds test against area i ) and returns
  the new count; the old count when the list holds none.
================
*/
ID_INLINE int AAS_ReachFaultDrop( const aasArea_t* areas, const int i, int* candidates, const int numCandidates )
{
	int k, n;

	for( k = numCandidates - 1; k >= 0; k-- )
	{
		if( ( areas[candidates[k]].flags & AREA_REACHABLE_WALK ) && AAS_ReachBoundsPass( &areas[i], &areas[candidates[k]] ) )
		{
			for( n = k; n + 1 < numCandidates; n++ )
			{
				candidates[n] = candidates[n + 1];
			}
			return numCandidates - 1;
		}
	}
	return numCandidates;
}

#endif /* !__AASREACH_FAST_H__ */
