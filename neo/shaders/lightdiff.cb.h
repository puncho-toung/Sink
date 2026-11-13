// lightdiff.cb.h - the push constants of the light-difference view ( track F, r_showLightDiff 2 ), declared once for
// the pixel stage. The backend's lightDiffConstants_t ( renderer/RenderBackend_lightlists.cpp ) repeats this layout
// under a static_assert: 32 bytes.

#ifndef LIGHTDIFF_CB_H
#define LIGHTDIFF_CB_H

#define LIGHTDIFF_BLOCK 16		// the reduction's block side, in pixels

struct LightDiffConstants
{
	int2	viewOrigin;		// the view's top-left pixel in the target
	int2	viewSize;
	uint	mode;			// 0 = the picture, 1 = the reduction
	float	scale;			// of the picture
	uint	pad0;
	uint	pad1;
};

#endif // LIGHTDIFF_CB_H
