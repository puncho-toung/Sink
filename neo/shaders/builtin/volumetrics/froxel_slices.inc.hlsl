// froxel_slices.inc.hlsl - the fog volume's slices, both ways ( track V, package V1 ): the one place that says where
// a slice lies. The kernels ( froxel_grid.inc.hlsl ) and the fog set's lookup ( froxel_fog.inc.hlsl ) call these two
// functions with the same two numbers: the bias S in units, and the step ln( 1 + farEnd / S ) / slices.
//
// Slices are slabs of constant distance along the view axis, biased logarithmic: about linear nearer than S and
// exponential beyond it. Slice 0 begins at the camera.

#ifndef FROXEL_SLICES_INC_HLSL
#define FROXEL_SLICES_INC_HLSL

// the far face of slice k, in units along the view axis; k = -1 gives 0, the camera
float FroxelSliceFarOf( float k, float sliceBias, float sliceStep )
{
	return sliceBias * ( exp( ( k + 1.0 ) * sliceStep ) - 1.0 );
}

// the slice coordinate of a distance along the view axis: 0 at the camera, k + 1 at the far face of slice k. Not
// capped: the caller caps it at the slice count
float FroxelSliceOf( float viewDepth, float sliceBias, float sliceStep )
{
	return log( 1.0 + max( viewDepth, 0.0 ) / sliceBias ) / sliceStep;
}

#endif // FROXEL_SLICES_INC_HLSL
