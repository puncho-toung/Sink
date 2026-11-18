//puncho - SB1b (R-SB1b-29, R-SB1b-30, R-SB1b-32): Stage A's structure id, the value the game's
// idRenderWorldLocal::GetMapCompileId computes from the loaded .proc. Both feed the SndStructure* helpers of
// snd_acousticfile.h the same integers in the same order: numAreas, numPortals, per portal its point count and
// area pair, per node its two children. The .proc itself is not read or changed here.
#include "precompiled.h"
#pragma hdrstop

#include "acoustic.h"

/*
==================
AcousticStructureNode_r

Walks the tree exactly as output.cpp's WriteNode_r writes it, so the i-th node fed is the i-th node of the
.proc and the game's areaNodes[i]. A leaf is reached only when the whole world pruned to one leaf: one node
-1 -1, whatever its area. A leaf child is -1 - area (a solid leaf, area -1, gives 0), a node child its
nodeNumber; like the writer, it descends only into a child whose number is > 0.
Mirrors output.cpp's WriteNode_r (and the NumberNodes_r numbering): a change there must be made here and in
idRenderWorldLocal::GetMapCompileId, or the two ids differ silently.
==================
*/
static void AcousticStructureNode_r( sndStructureHash_t& state, const node_t* node )
{
	if( node->planenum == PLANENUM_LEAF )
	{
		SndStructureNode( state, -1, -1 );
		return;
	}

	int child[2];
	for( int i = 0; i < 2; i++ )
	{
		if( node->children[i]->planenum == PLANENUM_LEAF )
		{
			child[i] = -1 - node->children[i]->area;
		}
		else
		{
			child[i] = node->children[i]->nodeNumber;
		}
	}

	SndStructureNode( state, child[0], child[1] );

	if( child[0] > 0 )
	{
		AcousticStructureNode_r( state, node->children[0] );
	}
	if( child[1] > 0 )
	{
		AcousticStructureNode_r( state, node->children[1] );
	}
}

/*
==================
AcousticStructureId

R-SB1b-30. Runs after WriteOutputFile: its WriteOutputNodes ran PruneNodes_r and NumberNodes_r on the world
tree, so nodeNumber is valid only from then on. The portals are read from interAreaPortals in list order with
the winding WriteOutputPortals chose. AcousticFinishStageA fails a map with no areas before calling this.
==================
*/
uint64 AcousticStructureId( const acousticMesh_t& mesh )
{
	assert( mesh.numAreas > 0 );
	assert( mesh.worldTree != NULL && mesh.worldTree->headnode != NULL );

	sndStructureHash_t state;
	SndStructureBegin( state, mesh.numAreas, interAreaPortals.Num() );

	for( int i = 0; i < interAreaPortals.Num(); i++ )
	{
		const interAreaPortal_t& iap = interAreaPortals[i];

		// RB's area portals carry their own winding in w; a brush side's portal uses the side's winding.
		// Mirrors output.cpp's WriteOutputPortals: a change there must be made here too
		const idWinding* w;
		if( iap.side )
		{
			w = iap.side->winding;
		}
		else
		{
			w = &iap.w;
		}

		SndStructurePortal( state, w->GetNumPoints(), iap.area0, iap.area1 );
	}

	AcousticStructureNode_r( state, mesh.worldTree->headnode );

	return SndStructureEnd( state );
}
