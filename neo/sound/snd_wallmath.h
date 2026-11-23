//puncho - W1-R: shared arithmetic only, also compiled by the offline mixer host.
#ifndef __SND_WALLMATH_H__
#define __SND_WALLMATH_H__

#include <float.h>
#include <math.h>

static inline bool SndWall_Finite( const float value )
{
	return value >= -FLT_MAX && value <= FLT_MAX;
}

// False invalidates the whole ray, including any surfaces already accumulated.
static inline bool SndWall_SurfaceLoss( const float transmission[2], float loss[2] )
{
	loss[0] = loss[1] = 0.0f;
	float result[2];
	for( int b = 0; b < 2; b++ )
	{
		if( !SndWall_Finite( transmission[b] ) || transmission[b] < 0.0f )
		{
			return false;
		}
		result[b] = transmission[b] <= 0.000001f ? 60.0f : -10.0f * log10f( transmission[b] );
		if( !SndWall_Finite( result[b] ) )
		{
			return false;
		}
		result[b] = result[b] < 0.0f ? 0.0f : ( result[b] > 60.0f ? 60.0f : result[b] );
	}
	loss[0] = result[0];
	loss[1] = result[1];
	return true;
}

static inline bool SndWall_RayTotal( const float sum[2], const float scale, const float limit, float loss[2] )
{
	loss[0] = loss[1] = 0.0f;
	if( !SndWall_Finite( scale ) || !SndWall_Finite( limit ) || scale < 0.0f || limit < 0.0f )
	{
		return false;
	}
	float result[2];
	for( int b = 0; b < 2; b++ )
	{
		if( !SndWall_Finite( sum[b] ) || sum[b] < 0.0f )
		{
			return false;
		}
		const float scaled = sum[b] * scale;
		if( !SndWall_Finite( scaled ) )
		{
			return false;
		}
		result[b] = scaled < limit ? scaled : limit;
	}
	loss[0] = result[0];
	loss[1] = result[1];
	return true;
}

// On any invalid input or intermediate, both emitter targets remain direct.
static inline bool SndWall_Targets( const float loss[][2], const int count, const float muffle,
	const float minCutoff, float& level, float& occlusion )
{
	level = occlusion = 0.0f;
	if( ( count != 1 && count != 4 ) || !SndWall_Finite( muffle ) || !SndWall_Finite( minCutoff ) ||
		muffle < 0.0f || minCutoff <= 0.0f )
	{
		return false;
	}
	float combined[2];
	for( int b = 0; b < 2; b++ )
	{
		float power = 0.0f;
		for( int i = 0; i < count; i++ )
		{
			if( !SndWall_Finite( loss[i][b] ) || loss[i][b] < 0.0f )
			{
				return false;
			}
			power += powf( 10.0f, -loss[i][b] * 0.1f );
		}
		if( power <= 0.0f || !SndWall_Finite( power ) )
		{
			return false;
		}
		combined[b] = -10.0f * log10f( power / count );
		if( !SndWall_Finite( combined[b] ) )
		{
			return false;
		}
		// Clear rays produce positive zero, including the all-clear case.
		if( combined[b] <= 0.0f ) { combined[b] = 0.0f; }
	}
	const float delta = muffle * ( combined[1] - combined[0] );
	if( !SndWall_Finite( delta ) ) { return false; }
	float filter = 0.0f;
	if( delta > 0.0f )
	{
		const float excess = powf( 10.0f, delta * 0.1f ) - 1.0f;
		if( !SndWall_Finite( excess ) ) { return false; }
		if( excess > 0.0f )
		{
			const float cutoff = 2500.0f / sqrtf( excess );
			if( !SndWall_Finite( cutoff ) ) { return false; }
			filter = 1000.0f / ( cutoff > minCutoff ? cutoff : minCutoff );
			if( !SndWall_Finite( filter ) ) { return false; }
		}
	}
	level = combined[0];
	occlusion = filter;
	return true;
}

#endif /* !__SND_WALLMATH_H__ */
