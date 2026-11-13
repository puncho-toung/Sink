// lightlist_heatmap.cb.h - the constants block the light lists' heatmap pass shares between its pixel shader and
// the backend ( renderer/RenderBackend_lightlists.cpp declares the same layout ).

#ifndef LIGHTLIST_HEATMAP_CB_H
#define LIGHTLIST_HEATMAP_CB_H

// the constants of the light lists' heatmap pass ( r_showLightLists 1 ): the view's Z-bin and tile layout as the
// frontend built it ( lightListView_t ), and where the view sits in its target
struct LightListHeatmapConstants
{
	uint	tileSize;		// pixels per tile side
	uint	tilesX;			// tiles per row
	uint	wordsPerTile;	// uint32 words of bits per tile
	uint	numBins;
	float	binsPerUnit;	// bins per unit of view distance
	float	zNear;
	float	opacity;		// of the heat colour over the image
	uint	pad;
	int2	viewOrigin;		// the view's top-left pixel in the target
	int2	viewSize;
};

#endif // LIGHTLIST_HEATMAP_CB_H
