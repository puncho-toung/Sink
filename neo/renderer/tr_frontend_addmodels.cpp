/*
===========================================================================

Doom 3 BFG Edition GPL Source Code
Copyright (C) 1993-2012 id Software LLC, a ZeniMax Media company.
Copyright (C) 2014-2024 Robert Beckebans
Copyright (C) 2014-2016 Kot in Action Creative Artel

This file is part of the Doom 3 BFG Edition GPL Source Code ("Doom 3 BFG Edition Source Code").

Doom 3 BFG Edition Source Code is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

Doom 3 BFG Edition Source Code is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with Doom 3 BFG Edition Source Code.  If not, see <http://www.gnu.org/licenses/>.

In addition, the Doom 3 BFG Edition Source Code is also subject to certain additional terms. You should have received a copy of these additional terms immediately following the terms and conditions of the GNU General Public License which accompanied the Doom 3 BFG Edition Source Code.  If not, please request a copy in writing from id Software at the address below.

If you have questions concerning this license or the applicable additional terms, you may contact in writing id Software LLC, c/o ZeniMax Media Inc., Suite 120, Rockville, Maryland 20850 USA.

===========================================================================
*/
#include "precompiled.h"
#pragma hdrstop

#if defined(USE_INTRINSICS_SSE)
	#if MOC_MULTITHREADED
		#include "CullingThreadPool.h"
	#else
		#include "../libs/moc/MaskedOcclusionCulling.h"
	#endif
#endif

#include "RenderCommon.h"
#include "Model_local.h"

idCVar r_skipStaticShadows( "r_skipStaticShadows", "0", CVAR_RENDERER | CVAR_BOOL, "skip static shadows" );
idCVar r_skipDynamicShadows( "r_skipDynamicShadows", "0", CVAR_RENDERER | CVAR_BOOL, "skip dynamic shadows" );
idCVar r_useParallelAddModels( "r_useParallelAddModels", "1", CVAR_RENDERER | CVAR_BOOL | CVAR_NOCHEAT, "add all models in parallel with jobs" );
idCVar r_useParallelAddShadows( "r_useParallelAddShadows", "1", CVAR_RENDERER | CVAR_INTEGER | CVAR_NOCHEAT, "0 = off, 1 = threaded", 0, 1 );
idCVar r_forceShadowCaps( "r_forceShadowCaps", "0", CVAR_RENDERER | CVAR_BOOL, "0 = skip rendering shadow caps if view is outside shadow volume, 1 = always render shadow caps" );
// RB begin
idCVar r_forceShadowMapsOnAlphaTestedSurfaces( "r_forceShadowMapsOnAlphaTestedSurfaces", "1", CVAR_RENDERER | CVAR_BOOL, "0 = same shadowing as with stencil shadows, 1 = ignore noshadows for alpha tested materials" );
// RB end
// foresthale 2014-11-24: cvar to control the material lod flags - this is the distance at which a mesh switches from lod1 to lod2, where lod3 will appear at this distance *2, lod4 at *4, and persistentLOD keyword will disable the max distance check (thus extending this LOD to all further distances, rather than disappearing)
idCVar r_lodMaterialDistance( "r_lodMaterialDistance", "500", CVAR_RENDERER | CVAR_FLOAT, "surfaces further than this distance will use lower quality versions (if their material uses the lod1-4 keywords, persistentLOD disables the max distance checks)" );

static const float CHECK_BOUNDS_EPSILON = 1.0f;



/*
==================
R_ClearEntityDefDynamicModel

If we know the reference bounds stays the same, we
only need to do this on entity update, not the full
R_FreeEntityDefDerivedData
==================
*/
void R_ClearEntityDefDynamicModel( idRenderEntityLocal* def )
{
	// free all the interaction surfaces
	for( idInteraction* inter = def->firstInteraction; inter != NULL && !inter->IsEmpty(); inter = inter->entityNext )
	{
		inter->FreeSurfaces();
	}

	// clear the dynamic model if present
	if( def->dynamicModel )
	{
		// this is copied from cachedDynamicModel, so it doesn't need to be freed
		def->dynamicModel = NULL;
	}
	def->dynamicModelFrameCount = 0;
	// Retain sibling storage through pose updates, exactly like cachedDynamicModel.
	for( int i = 0; i < 3; i++ )
	{
		def->lodDynamicModelFrameCount[i] = 0;
	}
}

/*
==================
R_IssueEntityDefCallback
==================
*/
bool R_IssueEntityDefCallback( idRenderEntityLocal* def )
{
	idBounds oldBounds = def->localReferenceBounds;

	bool update;
	if( tr.viewDef != NULL )
	{
		update = def->parms.callback( &def->parms, &tr.viewDef->renderView );
	}
	else
	{
		update = def->parms.callback( &def->parms, NULL );
	}
	tr.pc.c_entityDefCallbacks++;

	if( def->parms.hModel == NULL )
	{
		common->Error( "R_IssueEntityDefCallback: dynamic entity callback didn't set model" );
	}

	if( r_checkBounds.GetBool() )
	{
		if(	oldBounds[0][0] > def->localReferenceBounds[0][0] + CHECK_BOUNDS_EPSILON ||
				oldBounds[0][1] > def->localReferenceBounds[0][1] + CHECK_BOUNDS_EPSILON ||
				oldBounds[0][2] > def->localReferenceBounds[0][2] + CHECK_BOUNDS_EPSILON ||
				oldBounds[1][0] < def->localReferenceBounds[1][0] - CHECK_BOUNDS_EPSILON ||
				oldBounds[1][1] < def->localReferenceBounds[1][1] - CHECK_BOUNDS_EPSILON ||
				oldBounds[1][2] < def->localReferenceBounds[1][2] - CHECK_BOUNDS_EPSILON )
		{
			common->Printf( "entity %i callback extended reference bounds\n", def->index );
		}
	}

	return update;
}

/*
===================
R_EntityDefDynamicModel

This is also called by the game code for idRenderWorldLocal::ModelTrace(), and idRenderWorldLocal::Trace() which is bad for performance...

Issues a deferred entity callback if necessary.
If the model isn't dynamic, it returns the original.
Returns the cached dynamic model if present, otherwise creates it.
===================
*/
idRenderModel* R_EntityDefDynamicModel( idRenderEntityLocal* def )
{
	if( def->dynamicModelFrameCount == tr.frameCount )
	{
		return def->dynamicModel;
	}

	// allow deferred entities to construct themselves
	bool callbackUpdate;
	if( def->parms.callback != NULL )
	{
		SCOPED_PROFILE_EVENT( "R_IssueEntityDefCallback" );
		callbackUpdate = R_IssueEntityDefCallback( def );
	}
	else
	{
		callbackUpdate = false;
	}

	idRenderModel* model = def->parms.hModel;

	if( model == NULL )
	{
		common->Error( "R_EntityDefDynamicModel: NULL model" );
		return NULL;
	}

	if( model->IsDynamicModel() == DM_STATIC )
	{
		def->dynamicModel = NULL;
		def->dynamicModelFrameCount = 0;
		return model;
	}

	// continously animating models (particle systems, etc) will have their snapshot updated every single view
	if( callbackUpdate || ( model->IsDynamicModel() == DM_CONTINUOUS && def->dynamicModelFrameCount != tr.frameCount ) )
	{
		R_ClearEntityDefDynamicModel( def );
	}

	// if we don't have a snapshot of the dynamic model, generate it now
	if( def->dynamicModel == NULL )
	{
		SCOPED_PROFILE_EVENT( "InstantiateDynamicModel" );

		// instantiate the snapshot of the dynamic model, possibly reusing memory from the cached snapshot
		def->cachedDynamicModel = model->InstantiateDynamicModel( &def->parms, tr.viewDef, def->cachedDynamicModel );

		if( def->cachedDynamicModel != NULL && r_checkBounds.GetBool() )
		{
			idBounds b = def->cachedDynamicModel->Bounds();
			if(	b[0][0] < def->localReferenceBounds[0][0] - CHECK_BOUNDS_EPSILON ||
					b[0][1] < def->localReferenceBounds[0][1] - CHECK_BOUNDS_EPSILON ||
					b[0][2] < def->localReferenceBounds[0][2] - CHECK_BOUNDS_EPSILON ||
					b[1][0] > def->localReferenceBounds[1][0] + CHECK_BOUNDS_EPSILON ||
					b[1][1] > def->localReferenceBounds[1][1] + CHECK_BOUNDS_EPSILON ||
					b[1][2] > def->localReferenceBounds[1][2] + CHECK_BOUNDS_EPSILON )
			{
				common->Printf( "entity %i dynamic model exceeded reference bounds\n", def->index );
			}
		}

		def->dynamicModel = def->cachedDynamicModel;
		def->dynamicModelFrameCount = tr.frameCount;
	}

	// set model depth hack value
	if( def->dynamicModel != NULL && model->DepthHack() != 0.0f && tr.viewDef != NULL )
	{
		idPlane eye, clip;
		idVec3 ndc;
		// the view is relative to the render origin, so the origin is made relative in double first
		const idVec3d& renderOrigin = tr.viewDef->renderOrigin;
		const idVec3 origin = renderOrigin.IsZero() ? def->parms.origin : ( idVec3d( def->parms.origin ) - renderOrigin ).ToVec3();
		R_TransformModelToClip( origin, tr.viewDef->worldSpace.modelViewMatrix, tr.viewDef->projectionMatrix, eye, clip );
		R_TransformClipToDevice( clip, ndc );
		// scaled by the distance from the far plane, in NDC: idRenderMatrix::ApplyModelDepthHack
		// moves z toward the near plane by this much
#if ID_REVERSED_Z
		def->parms.modelDepthHack = model->DepthHack() * ndc.z;
#else
		def->parms.modelDepthHack = model->DepthHack() * ( 1.0f - ndc.z );
#endif
	}
	else
	{
		def->parms.modelDepthHack = 0.0f;
	}

	return def->dynamicModel;
}

/*
===================
R_SetupDrawSurfShader
===================
*/
void R_SetupDrawSurfShader( drawSurf_t* drawSurf, const idMaterial* shader, const renderEntity_t* renderEntity )
{
	drawSurf->material = shader;
	drawSurf->sort = shader->GetSort();

	// process the shader expressions for conditionals / color / texcoords
	const float*	constRegs = shader->ConstantRegisters();
	if( likely( constRegs != NULL ) )
	{
		// shader only uses constant values
		drawSurf->shaderRegisters = constRegs;
	}
	else
	{
		// by default evaluate with the entityDef's shader parms
		const float* shaderParms = renderEntity->shaderParms;

		// a reference shader will take the calculated stage color value from another shader
		// and use that for the parm0-parm3 of the current shader, which allows a stage of
		// a light model and light flares to pick up different flashing tables from
		// different light shaders
		float generatedShaderParms[MAX_ENTITY_SHADER_PARMS];
		if( unlikely( renderEntity->referenceShader != NULL ) )
		{
			// evaluate the reference shader to find our shader parms
			float refRegs[MAX_EXPRESSION_REGISTERS];
			renderEntity->referenceShader->EvaluateRegisters( refRegs, renderEntity->shaderParms,
					tr.viewDef->renderView.shaderParms,
					tr.viewDef->renderView.time[renderEntity->timeGroup] * 0.001f, renderEntity->referenceSound );

			const shaderStage_t* pStage = renderEntity->referenceShader->GetStage( 0 );

			memcpy( generatedShaderParms, renderEntity->shaderParms, sizeof( generatedShaderParms ) );
			generatedShaderParms[0] = refRegs[ pStage->color.registers[0] ];
			generatedShaderParms[1] = refRegs[ pStage->color.registers[1] ];
			generatedShaderParms[2] = refRegs[ pStage->color.registers[2] ];

			shaderParms = generatedShaderParms;
		}

		// allocate frame memory for the shader register values
		float* regs = ( float* )R_FrameAlloc( shader->GetNumRegisters() * sizeof( float ), FRAME_ALLOC_SHADER_REGISTER );
		drawSurf->shaderRegisters = regs;

		// process the shader expressions for conditionals / color / texcoords
		shader->EvaluateRegisters( regs, shaderParms, tr.viewDef->renderView.shaderParms,
								   tr.viewDef->renderView.time[renderEntity->timeGroup] * 0.001f, renderEntity->referenceSound );
	}
}

/*
===================
R_SetupDrawSurfJoints
===================
*/
void R_SetupDrawSurfJoints( drawSurf_t* drawSurf, const srfTriangles_t* tri, const idMaterial* shader, nvrhi::ICommandList* commandList )
{
	// RB: added check wether GPU skinning is available at all
	if( tri->staticModelWithJoints == NULL || !r_useGPUSkinning.GetBool() )
	{
		drawSurf->jointCache = 0;
		return;
	}
	// RB end

	idRenderModelStatic* model = tri->staticModelWithJoints;
	assert( model->jointsInverted != NULL );

	if( !vertexCache.CacheIsCurrent( model->jointsInvertedBuffer ) )
	{
		model->jointsInvertedBuffer = vertexCache.AllocJoint( model->jointsInverted, model->numInvertedJoints, sizeof( idJointMat ), commandList );
	}
	drawSurf->jointCache = model->jointsInvertedBuffer;
}

/*
===================
R_AddSingleModel

May be run in parallel.

Here is where dynamic models actually get instantiated, and necessary
interaction surfaces get created. This is all done on a sort-by-model
basis to keep source data in cache (most likely L2) as any interactions
and shadows are generated, since dynamic models will typically be lit by
two or more lights.
===================
*/
void R_AddSingleModel( viewEntity_t* vEntity )
{
	// we will add all interaction surfs here, to be chained to the lights in later serial code
	vEntity->drawSurfs = NULL;
	vEntity->contactedCapHits = 0;

	// globals we really should pass in...
	const viewDef_t* viewDef = tr.viewDef;

	idRenderEntityLocal* entityDef = vEntity->entityDef;
	const renderEntity_t* renderEntity = &entityDef->parms;
	const idRenderWorldLocal* world = entityDef->world;

	if( viewDef->isXraySubview && entityDef->parms.xrayIndex == 1 )
	{
		return;
	}
	else if( !viewDef->isXraySubview && entityDef->parms.xrayIndex == 2 )
	{
		return;
	}

	SCOPED_PROFILE_EVENT( renderEntity->hModel == NULL ? "Unknown Model" : renderEntity->hModel->Name() );

	// calculate the znear for testing whether or not the view is inside a shadow projection
	const float znear = ( viewDef->renderView.cramZNear ) ? ( r_znear.GetFloat() * 0.25f ) : r_znear.GetFloat();

	// if the entity wasn't seen through a portal chain, it was added just for light shadows
	const bool modelIsVisible = !vEntity->scissorRect.IsEmpty();
	const bool addInteractions = modelIsVisible && ( !viewDef->isXraySubview || entityDef->parms.xrayIndex == 2 );
	const int entityIndex = entityDef->index;

	//---------------------------
	// Find which of the visible lights contact this entity
	//
	// If the entity doesn't accept light or cast shadows from any surface,
	// this can be skipped.
	//
	// OPTIMIZE: world areas can assume all referenced lights are used
	//---------------------------
	int	numContactedLights = 0;
	static const int MAX_CONTACTED_LIGHTS = 128;
	viewLight_t* contactedLights[MAX_CONTACTED_LIGHTS];
	idInteraction* staticInteractions[MAX_CONTACTED_LIGHTS];

	if( renderEntity->hModel == NULL ||
			renderEntity->hModel->ModelHasInteractingSurfaces() ||
			renderEntity->hModel->ModelHasShadowCastingSurfaces() )
	{
		SCOPED_PROFILE_EVENT( "Find lights" );
		for( viewLight_t* vLight = viewDef->viewLights; vLight != NULL; vLight = vLight->next )
		{
			if( vLight->scissorRect.IsEmpty() )
			{
				continue;
			}
			if( vLight->entityInteractionState != NULL )
			{
				// new code path, everything was done in AddLight
				if( vLight->entityInteractionState[entityIndex] == viewLight_t::INTERACTION_YES )
				{
					contactedLights[numContactedLights] = vLight;
					staticInteractions[numContactedLights] = world->GetInteraction( vLight->lightDef->index, entityIndex );
					if( ++numContactedLights == MAX_CONTACTED_LIGHTS )
					{
						vEntity->contactedCapHits = 1;
						break;
					}
				}
				continue;
			}

			const idRenderLightLocal* lightDef = vLight->lightDef;

			if( !lightDef->globalLightBounds.IntersectsBounds( entityDef->globalReferenceBounds ) )
			{
				continue;
			}

			if( R_CullModelBoundsToLight( lightDef, entityDef->localReferenceBounds, entityDef->modelRenderMatrix ) )
			{
				continue;
			}

			if( !modelIsVisible )
			{
				// some lights have their center of projection outside the world
				if( lightDef->areaNum != -1 )
				{
					// if no part of the model is in an area that is connected to
					// the light center (it is behind a solid, closed door), we can ignore it
					bool areasConnected = false;
					for( areaReference_t* ref = entityDef->entityRefs; ref != NULL; ref = ref->ownerNext )
					{
						if( world->AreasAreConnected( lightDef->areaNum, ref->area->areaNum, PS_BLOCK_VIEW ) )
						{
							areasConnected = true;
							break;
						}
					}
					if( areasConnected == false )
					{
						// can't possibly be seen or shadowed
						continue;
					}
				}

				// check more precisely for shadow visibility
				idBounds shadowBounds;
				R_ShadowBounds( entityDef->globalReferenceBounds, lightDef->globalLightBounds, lightDef->globalLightOrigin, shadowBounds );

				// this doesn't say that the shadow can't effect anything, only that it can't
				// effect anything in the view
				if( idRenderMatrix::CullBoundsToMVP( viewDef->worldCullMVP, shadowBounds ) )
				{
					continue;
				}
			}
			contactedLights[numContactedLights] = vLight;
			staticInteractions[numContactedLights] = world->GetInteraction( vLight->lightDef->index, entityIndex );
			if( ++numContactedLights == MAX_CONTACTED_LIGHTS )
			{
				vEntity->contactedCapHits = 1;
				break;
			}
		}
	}

	// if we aren't visible and none of the shadows stretch into the view,
	// we don't need to do anything else
	if( !modelIsVisible && numContactedLights == 0 )
	{
		return;
	}

	//---------------------------
	// create a dynamic model if the geometry isn't static
	//---------------------------
	idRenderModel* model = R_EntityDefDynamicModel( entityDef );
	if( model == NULL || model->NumSurfaces() <= 0 )
	{
		return;
	}

	// Rendering alone may use a sibling. R_EntityDefDynamicModel and every trace retain LOD0.
	idRenderModel* baseGeometry = model;
	int lodLevel = 0;
	idRenderModel* source = entityDef->parms.hModel;
	if( source->NumLods() > 0 && !renderEntity->weaponDepthHack && renderEntity->modelDepthHack == 0.0f )
	{
		const float scale = ( renderEntity->lodScale > 0.0f ? renderEntity->lodScale : 1.0f ) * viewDef->modelLodScale;
		const idVec3 size = entityDef->globalReferenceBounds[1] - entityDef->globalReferenceBounds[0];
		float projectedHeight = 0.0f;
		for( int axis = 0; axis < 3; axis++ )
		{
			projectedHeight += size[axis] * idMath::Fabs( viewDef->renderView.viewaxis[2][axis] );
		}
		const float diameter = size.Length();
		const float pixels = diameter > 0.0f ? R_ProjectedBoundsPixels( entityDef->globalReferenceBounds, viewDef ) * projectedHeight / diameter : 0.0f;
		for( int n = 0; n < source->NumLods(); n++ )
		{
			const float hysteresis = viewDef->isSubview ? 1.0f : ( entityDef->lodLevel > n ? 1.1f : 0.9f );
			if( pixels < viewDef->modelLodPixels[n] * scale * hysteresis )
			{
				lodLevel = n + 1;
			}
		}
		if( !viewDef->isSubview && lodLevel != entityDef->lodLevel )
		{
			vEntity->lodSwitches = 1;
			vEntity->lodThrash = tr.frameCount - entityDef->lodSwitchFrame < 30;
			entityDef->lodSwitchFrame = tr.frameCount;
			entityDef->lodLevel = lodLevel;
		}
		if( lodLevel > 0 )
		{
			idRenderModel* sibling = source->Lod( lodLevel );
			if( sibling->IsDynamicModel() != DM_STATIC )
			{
				//puncho - model LOD (P2 review): one snapshot per level, skinned once per frame as stock skins
				// cachedDynamicModel. A view and a subview that pick different levels in one frame each keep their
				// own, so neither deletes the model the other's drawSurfs point into, nor rebuilds it on the heap
				const int slot = lodLevel - 1;
				if( entityDef->lodDynamicModelFrameCount[slot] != tr.frameCount )
				{
					vEntity->lodCacheBuilds += entityDef->lodDynamicModel[slot] == NULL;
					entityDef->lodDynamicModel[slot] = sibling->InstantiateDynamicModel( renderEntity, viewDef, entityDef->lodDynamicModel[slot] );
					entityDef->lodDynamicModelFrameCount[slot] = tr.frameCount;
				}
				model = entityDef->lodDynamicModel[slot];
			}
			else
			{
				model = sibling;
			}
			if( model == NULL )
			{
				model = baseGeometry;
				lodLevel = 0;
			}
		}
	}
	vEntity->lodLevel = lodLevel;
	if( lodLevel > 0 )
	{
		//puncho - model LOD (P2 review): binary static models may store a surface without geometry
		for( int i = 0; i < baseGeometry->NumSurfaces(); i++ )
		{
			const srfTriangles_t* geometry = baseGeometry->Surface( i )->geometry;
			vEntity->lodTrisSaved += geometry != NULL ? geometry->numIndexes / 3 : 0;
		}
		for( int i = 0; i < model->NumSurfaces(); i++ )
		{
			const srfTriangles_t* geometry = model->Surface( i )->geometry;
			vEntity->lodTrisSaved -= geometry != NULL ? geometry->numIndexes / 3 : 0;
		}
		// Static interaction surface indices belong to the base mesh, never to its sibling.
		for( int i = 0; i < numContactedLights; i++ )
		{
			staticInteractions[i] = NULL;
		}
	}

	// add the lightweight blood decal surfaces if the model is directly visible
	if( modelIsVisible && lodLevel == 0 )
	{
		assert( !vEntity->scissorRect.IsEmpty() );

		if( entityDef->decals != NULL && !r_skipDecals.GetBool() )
		{
			entityDef->decals->CreateDeferredDecals( model );

			unsigned int numDrawSurfs = entityDef->decals->GetNumDecalDrawSurfs();
			for( unsigned int i = 0; i < numDrawSurfs; i++ )
			{
				drawSurf_t* decalDrawSurf = entityDef->decals->CreateDecalDrawSurf( vEntity, i );
				if( decalDrawSurf != NULL )
				{
					decalDrawSurf->linkChain = NULL;
					decalDrawSurf->nextOnLight = vEntity->drawSurfs;
					vEntity->drawSurfs = decalDrawSurf;
				}
			}
		}

		if( entityDef->overlays != NULL && !r_skipOverlays.GetBool() )
		{
			entityDef->overlays->CreateDeferredOverlays( model );

			unsigned int numDrawSurfs = entityDef->overlays->GetNumOverlayDrawSurfs();
			for( unsigned int i = 0; i < numDrawSurfs; i++ )
			{
				drawSurf_t* overlayDrawSurf = entityDef->overlays->CreateOverlayDrawSurf( vEntity, model, i );
				if( overlayDrawSurf != NULL )
				{
					overlayDrawSurf->linkChain = NULL;
					overlayDrawSurf->nextOnLight = vEntity->drawSurfs;
					vEntity->drawSurfs = overlayDrawSurf;
				}
			}
		}
	}

	//---------------------------
	// copy matrix related stuff for back-end use
	// and setup a render matrix for faster culling
	//---------------------------
	vEntity->modelDepthHack = renderEntity->modelDepthHack;
	vEntity->weaponDepthHack = renderEntity->weaponDepthHack;
	vEntity->skipMotionBlur = renderEntity->skipMotionBlur;

	// the absolute matrix for the frontend, and the matrix relative to the render origin for the
	// model view and the backend; at a zero origin both are the def's matrix
	const idVec3d& renderOrigin = viewDef->renderOrigin;
	memcpy( vEntity->modelMatrixAbs, entityDef->modelMatrix, sizeof( vEntity->modelMatrixAbs ) );
	memcpy( vEntity->modelMatrixRel, vEntity->modelMatrixAbs, sizeof( vEntity->modelMatrixRel ) );
	if( !renderOrigin.IsZero() )
	{
		// only the translation moves, subtracted in double from the def's own matrix
		vEntity->modelMatrixRel[3 * 4 + 0] = float( double( vEntity->modelMatrixAbs[3 * 4 + 0] ) - renderOrigin.x );
		vEntity->modelMatrixRel[3 * 4 + 1] = float( double( vEntity->modelMatrixAbs[3 * 4 + 1] ) - renderOrigin.y );
		vEntity->modelMatrixRel[3 * 4 + 2] = float( double( vEntity->modelMatrixAbs[3 * 4 + 2] ) - renderOrigin.z );
	}
	R_MatrixMultiply( vEntity->modelMatrixRel, viewDef->worldSpace.modelViewMatrix, vEntity->modelViewMatrix );

	idRenderMatrix viewMat;
	idRenderMatrix::Transpose( *( idRenderMatrix* )vEntity->modelViewMatrix, viewMat );
	idRenderMatrix::Multiply( viewDef->projectionRenderMatrix, viewMat, vEntity->mvp );
	idRenderMatrix::Multiply( viewDef->unjitteredProjectionRenderMatrix, viewMat, vEntity->unjitteredMVP );
	if( renderEntity->weaponDepthHack )
	{
		idRenderMatrix::ApplyDepthHack( vEntity->mvp );
	}
	if( renderEntity->modelDepthHack != 0.0f )
	{
		idRenderMatrix::ApplyModelDepthHack( vEntity->mvp, renderEntity->modelDepthHack );
	}

	// local light and view origins are used to determine if the view is definitely outside
	// an extruded shadow volume, which means we can skip drawing the end caps.
	// The eye relative to the render origin, in the relative matrix's local space: the same
	// point as the absolute pair gives, and the value the backend uploads
	const idVec3 eye = renderOrigin.IsZero() ? viewDef->renderView.vieworg : ( idVec3d( viewDef->renderView.vieworg ) - renderOrigin ).ToVec3();
	R_GlobalPointToLocal( vEntity->modelMatrixRel, eye, vEntity->localViewOrigin );
	const idVec3& localViewOrigin = vEntity->localViewOrigin;

	//---------------------------
	// add all the model surfaces
	//---------------------------
	for( int surfaceNum = 0; surfaceNum < model->NumSurfaces(); surfaceNum++ )
	{
		const modelSurface_t* surf = model->Surface( surfaceNum );

		// for debugging, only show a single surface at a time
		if( r_singleSurface.GetInteger() >= 0 && surfaceNum != r_singleSurface.GetInteger() )
		{
			continue;
		}

		srfTriangles_t* tri = surf->geometry;
		if( tri == NULL )
		{
			continue;
		}
		if( tri->numIndexes == 0 )
		{
			continue;		// happens for particles
		}
		const idMaterial* shader = surf->shader;
		if( shader == NULL )
		{
			continue;
		}

		// motorsep 11-24-2014; checking for LOD surface for LOD1 iteration
		if( shader->IsLOD() )
		{
			// foresthale 2014-11-24: calculate the bounds and get the distance from camera to bounds
			// ( a copy: tri->bounds is shared by every view and job that draws this model )
			idBounds localBounds = tri->bounds;
			if( tri->staticModelWithJoints )
			{
				// skeletal models have difficult to compute bounds for surfaces, so use the whole entity
				localBounds = vEntity->entityDef->localReferenceBounds;
			}
			const float* bounds = localBounds.ToFloatPtr();
			idVec3 nearestPointOnBounds = localViewOrigin;
			nearestPointOnBounds.x = Max( nearestPointOnBounds.x, bounds[0] );
			nearestPointOnBounds.x = Min( nearestPointOnBounds.x, bounds[3] );
			nearestPointOnBounds.y = Max( nearestPointOnBounds.y, bounds[1] );
			nearestPointOnBounds.y = Min( nearestPointOnBounds.y, bounds[4] );
			nearestPointOnBounds.z = Max( nearestPointOnBounds.z, bounds[2] );
			nearestPointOnBounds.z = Min( nearestPointOnBounds.z, bounds[5] );
			idVec3 delta = nearestPointOnBounds - localViewOrigin;
			float distance = delta.LengthFast();

			if( !shader->IsLODVisibleForDistance( distance, r_lodMaterialDistance.GetFloat() ) )
			{
				continue;
			}
		}

		// foresthale 2014-09-01: don't skip surfaces that use the "forceShadows" flag
		if( !shader->IsDrawn() && !shader->SurfaceCastsShadow() )
		{
			continue;		// collision hulls, etc
		}

		// RemapShaderBySkin
		if( entityDef->parms.customShader != NULL )
		{
			// this is sort of a hack, but causes deformed surfaces to map to empty surfaces,
			// so the item highlight overlay doesn't highlight the autosprite surface
			if( shader->Deform() )
			{
				continue;
			}
			shader = entityDef->parms.customShader;
		}
		else if( entityDef->parms.customSkin )
		{
			shader = entityDef->parms.customSkin->RemapShaderBySkin( shader );
			if( shader == NULL )
			{
				continue;
			}
			// foresthale 2014-09-01: don't skip surfaces that use the "forceShadows" flag
			if( !shader->IsDrawn() && !shader->SurfaceCastsShadow() )
			{
				continue;
			}
		}

		// optionally override with the renderView->globalMaterial
		if( tr.primaryRenderView.globalMaterial != NULL )
		{
			shader = tr.primaryRenderView.globalMaterial;
		}

		SCOPED_PROFILE_EVENT( shader->GetName() );

		// debugging tool to make sure we have the correct pre-calculated bounds
		if( r_checkBounds.GetBool() )
		{
			for( int j = 0; j < tri->numVerts; j++ )
			{
				int k;
				for( k = 0; k < 3; k++ )
				{
					if( tri->verts[j].xyz[k] > tri->bounds[1][k] + CHECK_BOUNDS_EPSILON
							|| tri->verts[j].xyz[k] < tri->bounds[0][k] - CHECK_BOUNDS_EPSILON )
					{
						common->Printf( "bad tri->bounds on %s:%s\n", entityDef->parms.hModel->Name(), shader->GetName() );
						break;
					}
					if( tri->verts[j].xyz[k] > entityDef->localReferenceBounds[1][k] + CHECK_BOUNDS_EPSILON
							|| tri->verts[j].xyz[k] < entityDef->localReferenceBounds[0][k] - CHECK_BOUNDS_EPSILON )
					{
						common->Printf( "bad referenceBounds on %s:%s\n", entityDef->parms.hModel->Name(), shader->GetName() );
						break;
					}
				}
				if( k != 3 )
				{
					break;
				}
			}
		}

		// view frustum culling for the precise surface bounds, which is tighter
		// than the entire entity reference bounds
		// If the entire model wasn't visible, there is no need to check the
		// individual surfaces.
		bool surfaceDirectlyVisible = modelIsVisible && !idRenderMatrix::CullBoundsToMVP( vEntity->mvp, tri->bounds );

		// RB: added check wether GPU skinning is available at all
		const bool gpuSkinned = ( tri->staticModelWithJoints != NULL && r_useGPUSkinning.GetBool() );

		//const char* shaderName = shader->GetName();
		//if( idStr::Cmp( shaderName, "textures/rock/sharprock_dark") == 0 )
		//{
		//	vEntity->mocTests += 0;
		//}

#if defined(USE_INTRINSICS_SSE)

		const idBounds& mocBounds = gpuSkinned ? entityDef->localReferenceBounds : tri->bounds;
		const bool viewInsideSurface = ( r_mocFixes.GetInteger() & 4 ) ?
			R_MocNearBounds( mocBounds, renderEntity->origin, renderEntity->axis, viewDef ) : tri->bounds.ContainsPoint( localViewOrigin );

		//if( viewInsideSurface && idStr::Cmp( shaderName, "models/weapons/berserk/fist") != 0 )
		//{
		//	vEntity->mocTests += 1;
		//
		//	tr.viewDef->renderWorld->DebugBounds( colorCyan, tri->bounds, renderEntity->origin );
		//}

		// RB: test surface visibility by drawing the triangles of the bounds
		if( r_useMaskedOcclusionCulling.GetBool() && !viewInsideSurface && !viewDef->isMirror && !viewDef->isSubview )
		{
			if( //!model->IsStaticWorldModel() &&
				!renderEntity->weaponDepthHack && renderEntity->modelDepthHack == 0.0f )
			{
				idVec4 triVerts[8];

				vEntity->mocIndexes += 36;
				vEntity->mocVerts += 8;

				idBounds surfaceBounds;
#if 1
				if( gpuSkinned )
				{
					surfaceBounds = vEntity->entityDef->localReferenceBounds;
				}
				else
#endif
				{
					surfaceBounds = tri->bounds;
				}

				idRenderMatrix modelRenderMatrix;
				idRenderMatrix::CreateFromOriginAxis( renderEntity->origin, renderEntity->axis, modelRenderMatrix );

				idRenderMatrix inverseBaseModelProject;
				idRenderMatrix::OffsetScaleForBounds( modelRenderMatrix, surfaceBounds, inverseBaseModelProject );

				idRenderMatrix invProjectMVPMatrix;
				idRenderMatrix::Multiply( viewDef->worldCullUnjitteredMVP, inverseBaseModelProject, invProjectMVPMatrix );

				vEntity->mocTests += 1;

				// NOTE: unit cube instead of zeroToOne cube
				idVec4* verts = tr.maskedUnitCubeVerts;
				for( int i = 0; i < 8; i++ )
				{
					// transform to clip space
					invProjectMVPMatrix.TransformPoint( verts[i], triVerts[i] );
				}


				// backface none so objects are still visible where we run into
#if MOC_MULTITHREADED
				tr.maskedOcclusionThreaded->SetMatrix( NULL );
				MaskedOcclusionCulling::CullingResult result = tr.maskedOcclusionThreaded->TestTriangles( ( float* )triVerts, tr.maskedZeroOneCubeIndexes, 12, MaskedOcclusionCulling::BACKFACE_NONE );
#else
				MaskedOcclusionCulling::CullingResult result = tr.maskedOcclusionCulling->TestTriangles( ( float* )triVerts, tr.maskedZeroOneCubeIndexes, 12, NULL, MaskedOcclusionCulling::BACKFACE_NONE );
#endif
				if( result != MaskedOcclusionCulling::VISIBLE )
				{
					vEntity->mocCulled += 1;
					surfaceDirectlyVisible = false;
				}
			}
		}
#endif // #if defined(USE_INTRINSICS_SSE)

		//--------------------------
		// base drawing surface
		//--------------------------
		const float* shaderRegisters = NULL;
		drawSurf_t* baseDrawSurf = NULL;
		if( surfaceDirectlyVisible && shader->IsDrawn() )
		{
			//puncho start - static arena (P2b.1.8): a stale static handle (its segment reset since) fails CacheIsCurrent,
			// which counts it in staticStaleHandles, and is re-uploaded here into this frame's buffers, never drawn. Dev:
			// r_staticTestStale plants one on a drawn surface, with the jobs off only: two entities sharing this tri in
			// parallel jobs could copy the planted handle between another job's test and its fallback
#if !defined( ID_RETAIL )
			if( vertexCache.staticTestStale.GetValue() > 0 && !r_useParallelAddModels.GetBool() )
			{
				vertexCache.PlantStaleHandle( tri->indexCache );
			}
#endif
			//puncho end
			// make sure we have an ambient cache and all necessary normals / tangents
			if( !vertexCache.CacheIsCurrent( tri->indexCache ) )
			{
				tri->indexCache = vertexCache.AllocIndex( tri->indexes, tri->numIndexes );
			}

			if( !vertexCache.CacheIsCurrent( tri->ambientCache ) )
			{
				// we are going to use it for drawing, so make sure we have the tangents and normals
				if( shader->ReceivesLighting() && !tri->tangentsCalculated )
				{
					assert( tri->staticModelWithJoints == NULL );
					R_DeriveTangents( tri );

					// RB: this was hit by parametric particle models ..
					//assert( false );	// this should no longer be hit
					// RB end
				}
				tri->ambientCache = vertexCache.AllocVertex( tri->verts, tri->numVerts );
			}

			// add the surface for drawing
			// we can re-use some of the values for light interaction surfaces
			baseDrawSurf = ( drawSurf_t* )R_FrameAlloc( sizeof( *baseDrawSurf ), FRAME_ALLOC_DRAW_SURFACE );
			baseDrawSurf->frontEndGeo = tri;
			baseDrawSurf->space = vEntity;
			baseDrawSurf->scissorRect = vEntity->scissorRect;
			baseDrawSurf->extraGLState = 0;

			R_SetupDrawSurfShader( baseDrawSurf, shader, renderEntity );

			shaderRegisters = baseDrawSurf->shaderRegisters;

			// Check for deformations (eyeballs, flares, etc)
			const deform_t shaderDeform = shader->Deform();
			if( shaderDeform != DFRM_NONE )
			{
				drawSurf_t* deformDrawSurf = R_DeformDrawSurf( baseDrawSurf );
				if( deformDrawSurf != NULL )
				{
					// any deforms may have created multiple draw surfaces
					for( drawSurf_t* surf = deformDrawSurf, * next = NULL; surf != NULL; surf = next )
					{
						next = surf->nextOnLight;

						surf->linkChain = NULL;
						surf->nextOnLight = vEntity->drawSurfs;
						vEntity->drawSurfs = surf;
					}
				}
			}

			// Most deform source surfaces do not need to be rendered.
			// However, particles are rendered in conjunction with the source surface.
			if( shaderDeform == DFRM_NONE || shaderDeform == DFRM_PARTICLE || shaderDeform == DFRM_PARTICLE2 )
			{
				// copy verts and indexes to this frame's hardware memory if they aren't already there
				if( !vertexCache.CacheIsCurrent( tri->ambientCache ) )
				{
					tri->ambientCache = vertexCache.AllocVertex( tri->verts, tri->numVerts );
				}
				if( !vertexCache.CacheIsCurrent( tri->indexCache ) )
				{
					tri->indexCache = vertexCache.AllocIndex( tri->indexes, tri->numIndexes );
				}

				R_SetupDrawSurfJoints( baseDrawSurf, tri, shader );

				baseDrawSurf->numIndexes = tri->numIndexes;
				baseDrawSurf->ambientCache = tri->ambientCache;
				baseDrawSurf->indexCache = tri->indexCache;

				baseDrawSurf->linkChain = NULL;		// link to the view
				baseDrawSurf->nextOnLight = vEntity->drawSurfs;
				vEntity->drawSurfs = baseDrawSurf;
			}

			// RB: use area the surface is in because a model can span multiple areas #965
			baseDrawSurf->area = NULL;

			if( shader->ReceivesLighting() )
			{
				idVec3 surfaceCenter;
				idVec3 triCenter = tri->bounds.GetCenter();

				idRenderMatrix modelRenderMatrix;
				idRenderMatrix::CreateFromOriginAxis( renderEntity->origin, renderEntity->axis, modelRenderMatrix );
				modelRenderMatrix.TransformPoint( triCenter, surfaceCenter );

				int surfaceArea = tr.primaryWorld->PointInArea( surfaceCenter );

				for( areaReference_t* ref = entityDef->entityRefs; ref != NULL; ref = ref->ownerNext )
				{
					idImage* lightGridImage = ref->area->lightGrid.GetIrradianceImage();

					if( surfaceArea == ref->area->areaNum && ref->area->lightGrid.lightGridPoints.Num() && lightGridImage != NULL && !lightGridImage->IsDefaulted() )
					{
						baseDrawSurf->area = ref->area;
						break;
					}
				}

				// RB: use first valid lightgrid
				// this would be wrong but less wrong than a flickering env_probe fallback
				if( baseDrawSurf->area == NULL )
				{
					for( areaReference_t* ref = entityDef->entityRefs; ref != NULL; ref = ref->ownerNext )
					{
						idImage* lightGridImage = ref->area->lightGrid.GetIrradianceImage();

						if( ref->area->lightGrid.lightGridPoints.Num() && lightGridImage != NULL && !lightGridImage->IsDefaulted() )
						{
							baseDrawSurf->area = ref->area;
							break;
						}
					}
				}

#if 0
				// show which area the surface is coming from
				//if( baseDrawSurf->area == NULL )
				{
					idBounds surfaceBounds;
					if( gpuSkinned )
					{
						surfaceBounds = vEntity->entityDef->localReferenceBounds;
					}
					else
					{
						surfaceBounds = tri->bounds;
					}

					idRenderMatrix modelRenderMatrix;
					idRenderMatrix::CreateFromOriginAxis( renderEntity->origin, renderEntity->axis, modelRenderMatrix );

					idRenderMatrix inverseBaseModelProject;
					idRenderMatrix::OffsetScaleForBounds( modelRenderMatrix, surfaceBounds, inverseBaseModelProject );

					// NOTE: unit cube instead of zeroToOne cube
					idVec4* verts = tr.maskedUnitCubeVerts;
					idVec4 triVerts[8];

					for( int i = 0; i < 8; i++ )
					{
						// transform to clip space
						inverseBaseModelProject.TransformPoint( verts[i], triVerts[i] );
					}

					static idVec4 colors[] = { colorBrown, colorBlue, colorCyan, colorGreen, colorYellow, colorRed, colorWhite };
					idVec4 color = colors[surfaceArea & 7];

					if( baseDrawSurf->area == NULL )
					{
						color = colorPurple;
					}

					// same as idRenderWorldLocal::DebugBox
					const int lifetime = 0;
					for( int i = 0; i < 4; i++ )
					{
						tr.viewDef->renderWorld->DebugLine( color, triVerts[i].ToVec3(), triVerts[( i + 1 ) & 3].ToVec3(), lifetime );
						tr.viewDef->renderWorld->DebugLine( color, triVerts[4 + i].ToVec3(), triVerts[4 + ( ( i + 1 ) & 3 )].ToVec3(), lifetime );
						tr.viewDef->renderWorld->DebugLine( color, triVerts[i].ToVec3(), triVerts[4 + i].ToVec3(), lifetime );
					}

					tr.viewDef->renderWorld->DebugAxis( surfaceCenter, renderEntity->axis );
				}
#endif
			}
		}

		//----------------------------------------
		// add all light interactions
		//----------------------------------------
		for( int contactedLight = 0; contactedLight < numContactedLights; contactedLight++ )
		{
			viewLight_t* vLight = contactedLights[contactedLight];
			const idRenderLightLocal* lightDef = vLight->lightDef;
			const idInteraction* interaction = staticInteractions[contactedLight];

			// check for a static interaction
			surfaceInteraction_t* surfInter = NULL;
			if( interaction > INTERACTION_EMPTY && interaction->staticInteraction )
			{
				// we have a static interaction that was calculated accurately
				assert( model->NumSurfaces() == interaction->numSurfaces );
				surfInter = &interaction->surfaces[surfaceNum];
			}
			else
			{
				// try to do a more precise cull of this model surface to the light
				if( R_CullModelBoundsToLight( lightDef, tri->bounds, entityDef->modelRenderMatrix ) )
				{
					continue;
				}
			}

			// "invisible ink" lights and shaders (imp spawn drawing on walls, etc)
			if( shader->Spectrum() != lightDef->lightShader->Spectrum() )
			{
				continue;
			}

			//--------------------------
			// surface light interactions
			//--------------------------

			if( addInteractions && surfaceDirectlyVisible && shader->ReceivesLighting() )
			{
				// static interactions can commonly find that no triangles from a surface
				// contact the light, even when the total model does
				//puncho - static arena (P2b.1.8): stage 4 inherits this: the light tris' static index cache (segment 1)
				// is used without CacheIsCurrent's generation test; P2b.4.11 adds it
				if( surfInter == NULL || surfInter->lightTrisIndexCache > 0 )
				{
					// make sure we have a valid shader register even if we didn't generate a drawn mesh above
					if( shaderRegisters == NULL )
					{
						drawSurf_t scratchSurf;
						R_SetupDrawSurfShader( &scratchSurf, shader, renderEntity );
						shaderRegisters = scratchSurf.shaderRegisters;
					}

					if( shaderRegisters != NULL )
					{
						// create a drawSurf for this interaction
						drawSurf_t* lightDrawSurf = ( drawSurf_t* )R_FrameAlloc( sizeof( *lightDrawSurf ), FRAME_ALLOC_DRAW_SURFACE );

						if( surfInter != NULL )
						{
							// optimized static interaction
							lightDrawSurf->numIndexes = surfInter->numLightTrisIndexes;
							lightDrawSurf->indexCache = surfInter->lightTrisIndexCache;
						}
						else
						{
							// throw the entire source surface at it without any per-triangle culling
							lightDrawSurf->numIndexes = tri->numIndexes;
							lightDrawSurf->indexCache = tri->indexCache;
						}

						lightDrawSurf->ambientCache = tri->ambientCache;
						lightDrawSurf->frontEndGeo = tri;
						lightDrawSurf->space = vEntity;
						lightDrawSurf->material = shader;
						lightDrawSurf->extraGLState = 0;
						lightDrawSurf->scissorRect = vLight->scissorRect; // interactionScissor;
						lightDrawSurf->sort = 0.0f;
						lightDrawSurf->shaderRegisters = shaderRegisters;

						R_SetupDrawSurfJoints( lightDrawSurf, tri, shader );

						// Determine which linked list to add the light surface to.
						// There will only be localSurfaces if the light casts shadows and
						// there are surfaces with NOSELFSHADOW.
						if( shader->Coverage() == MC_TRANSLUCENT )
						{
							lightDrawSurf->linkChain = &vLight->translucentInteractions;
						}
						else if( !lightDef->parms.noShadows && shader->TestMaterialFlag( MF_NOSELFSHADOW ) )
						{
							lightDrawSurf->linkChain = &vLight->localInteractions;
						}
						else
						{
							lightDrawSurf->linkChain = &vLight->globalInteractions;
						}
						lightDrawSurf->nextOnLight = vEntity->drawSurfs;
						vEntity->drawSurfs = lightDrawSurf;
					}
				}
			}

			//--------------------------
			// surface shadows
			//--------------------------

#if 1
			if( !shader->SurfaceCastsShadow() && !( r_forceShadowMapsOnAlphaTestedSurfaces.GetBool() && shader->Coverage() == MC_PERFORATED ) )
			{
				continue;
			}
#else
			// Steel Storm 2 behaviour - this destroys many alpha tested shadows in vanilla BFG

			// motorsep 11-08-2014; if r_forceShadowMapsOnAlphaTestedSurfaces is 0 when shadow mapping is on,
			// don't render shadows from all alphaTest surfaces.
			// Useful as global performance booster for old GPUs to disable shadows from grass/foliage/etc.
			if( r_useShadowMapping.GetBool() )
			{
				if( shader->Coverage() == MC_PERFORATED )
				{
					if( !r_forceShadowMapsOnAlphaTestedSurfaces.GetBool() )
					{
						continue;
					}
				}
			}

			// if material has "noShadows" global key
			if( !shader->SurfaceCastsShadow() )
			{
				// motorsep 11-08-2014; if r_forceShadowMapsOnAlphaTestedSurfaces is 1 when shadow mapping is on,
				// check if a surface IS NOT alphaTested and has "noShadows" global key;
				// or if a surface IS alphaTested and has "noShadows" global key;
				// if either is true, don't make surfaces cast shadow maps.
				if( r_useShadowMapping.GetBool() )
				{
					if( shader->Coverage() != MC_PERFORATED && shader->TestMaterialFlag( MF_NOSHADOWS ) )
					{
						continue;
					}
					else if( shader->Coverage() == MC_PERFORATED && shader->TestMaterialFlag( MF_NOSHADOWS ) )
					{
						continue;
					}
				}
				else
				{
					continue;
				}
			}
#endif

			if( !lightDef->LightCastsShadows() )
			{
				continue;
			}

			// some entities, like view weapons, don't cast any shadows
			if( entityDef->parms.noShadow )
			{
				continue;
			}

			// No shadow if it's suppressed for this light.
			if( entityDef->parms.suppressShadowInLightID && entityDef->parms.suppressShadowInLightID == lightDef->parms.lightId )
			{
				continue;
			}


			// RB: draw shadow occluder using shadow mapping
			// OPTIMIZE: check if projected occluder box intersects the view
			//
			//if( addInteractions && surfaceDirectlyVisible && shader->ReceivesLighting() )
			{
				// static interactions can commonly find that no triangles from a surface
				// contact the light, even when the total model does
				//puncho - static arena (P2b.1.8): stage 4 inherits this: the light tris' static index cache (segment 1)
				// is used without CacheIsCurrent's generation test; P2b.4.11 adds it
				if( surfInter == NULL || surfInter->lightTrisIndexCache > 0 )
				{
					// create a drawSurf for this interaction
					drawSurf_t* shadowDrawSurf = ( drawSurf_t* )R_FrameAlloc( sizeof( *shadowDrawSurf ), FRAME_ALLOC_DRAW_SURFACE );

					if( surfInter != NULL )
					{
						// optimized static interaction
						shadowDrawSurf->numIndexes = surfInter->numLightTrisIndexes;
						shadowDrawSurf->indexCache = surfInter->lightTrisIndexCache;
					}
					else
					{
						// make sure we have an ambient cache and all necessary normals / tangents
						if( !vertexCache.CacheIsCurrent( tri->indexCache ) )
						{
							tri->indexCache = vertexCache.AllocIndex( tri->indexes, tri->numIndexes );
						}

						// throw the entire source surface at it without any per-triangle culling
						shadowDrawSurf->numIndexes = tri->numIndexes;
						shadowDrawSurf->indexCache = tri->indexCache;
					}

					if( !vertexCache.CacheIsCurrent( tri->ambientCache ) )
					{
						// we are going to use it for drawing, so make sure we have the tangents and normals
						if( shader->ReceivesLighting() && !tri->tangentsCalculated )
						{
							assert( tri->staticModelWithJoints == NULL );
							R_DeriveTangents( tri );

							// RB: this was hit by parametric particle models ..
							//assert( false );	// this should no longer be hit
							// RB end
						}
						tri->ambientCache = vertexCache.AllocVertex( tri->verts, tri->numVerts );
					}

					shadowDrawSurf->ambientCache = tri->ambientCache;
					shadowDrawSurf->frontEndGeo = tri;
					shadowDrawSurf->space = vEntity;
					shadowDrawSurf->material = shader;
					shadowDrawSurf->extraGLState = 0;
					shadowDrawSurf->scissorRect = vLight->scissorRect; // interactionScissor;
					shadowDrawSurf->sort = 0.0f;
					//shadowDrawSurf->shaderRegisters = baseDrawSurf->shaderRegisters; // TODO FIXME

					if( shader->Coverage() == MC_PERFORATED )
					{
						R_SetupDrawSurfShader( shadowDrawSurf, shader, renderEntity );
					}

					R_SetupDrawSurfJoints( shadowDrawSurf, tri, shader );

					// determine which linked list to add the shadow surface to

					//shadowDrawSurf->linkChain = shader->TestMaterialFlag( MF_NOSELFSHADOW ) ? &vLight->localShadows : &vLight->globalShadows;

					shadowDrawSurf->linkChain = &vLight->globalShadows;
					shadowDrawSurf->nextOnLight = vEntity->drawSurfs;

					vEntity->drawSurfs = shadowDrawSurf;

				}
			}
			// RB end
		}
	}
}

REGISTER_PARALLEL_JOB( R_AddSingleModel, "R_AddSingleModel" );

/*
=================
R_LinkDrawSurfToView

Als called directly by GuiModel
=================
*/
void R_LinkDrawSurfToView( drawSurf_t* drawSurf, viewDef_t* viewDef )
{
	// if it doesn't fit, resize the list
	if( viewDef->numDrawSurfs == viewDef->maxDrawSurfs )
	{
		drawSurf_t** old = viewDef->drawSurfs;
		int count;

		if( viewDef->maxDrawSurfs == 0 )
		{
			viewDef->maxDrawSurfs = INITIAL_DRAWSURFS;
			count = 0;
		}
		else
		{
			count = viewDef->maxDrawSurfs * sizeof( viewDef->drawSurfs[0] );
			viewDef->maxDrawSurfs *= 2;
		}
		viewDef->drawSurfs = ( drawSurf_t** )R_FrameAlloc( viewDef->maxDrawSurfs * sizeof( viewDef->drawSurfs[0] ), FRAME_ALLOC_DRAW_SURFACE_POINTER );
		memcpy( viewDef->drawSurfs, old, count );
	}

	viewDef->drawSurfs[viewDef->numDrawSurfs] = drawSurf;
	viewDef->numDrawSurfs++;
}

/*
===================
R_AddModels

The end result of running this is the addition of drawSurf_t to the
tr.viewDef->drawSurfs[] array and light link chains, along with
frameData and vertexCache allocations to support the drawSurfs.
===================
*/
void R_AddModels()
{
	SCOPED_PROFILE_EVENT( "R_AddModels" );

	// RB: already done in R_FillMaskedOcclusionBufferWithModels
	// tr.viewDef->viewEntitys = R_SortViewEntities( tr.viewDef->viewEntitys );

	//-------------------------------------------------
	// Go through each view entity that is either visible to the view, or to
	// any light that intersects the view (for shadows).
	//-------------------------------------------------

	if( r_useParallelAddModels.GetBool() )
	{
		for( viewEntity_t* vEntity = tr.viewDef->viewEntitys; vEntity != NULL; vEntity = vEntity->next )
		{
			tr.frontEndJobList->AddJob( ( jobRun_t )R_AddSingleModel, vEntity );
		}
		tr.frontEndJobList->Submit();
		tr.frontEndJobList->Wait();
	}
	else
	{
		for( viewEntity_t* vEntity = tr.viewDef->viewEntitys; vEntity != NULL; vEntity = vEntity->next )
		{
			R_AddSingleModel( vEntity );
		}
	}

	//-------------------------------------------------
	// Move the draw surfs to the view.
	//-------------------------------------------------

	tr.viewDef->numDrawSurfs = 0;	// clear the ambient surface list
	tr.viewDef->maxDrawSurfs = 0;	// will be set to INITIAL_DRAWSURFS on R_LinkDrawSurfToView

	for( viewEntity_t* vEntity = tr.viewDef->viewEntitys; vEntity != NULL; vEntity = vEntity->next )
	{
		tr.pc.c_contactedCapHits += vEntity->contactedCapHits;
		tr.pc.c_lodEntities1 += vEntity->lodLevel == 1;
		tr.pc.c_lodEntities2 += vEntity->lodLevel == 2;
		tr.pc.c_lodEntities3 += vEntity->lodLevel == 3;
		tr.pc.c_lodTrisSaved += vEntity->lodTrisSaved;
		tr.pc.c_lodSwitches += vEntity->lodSwitches;
		tr.pc.c_lodThrash += vEntity->lodThrash;
		tr.pc.c_lodCacheBuilds += vEntity->lodCacheBuilds;
		if( r_showModelLod.GetBool() )
		{
			tr.viewDef->renderWorld->DrawText( va( "LOD %d", vEntity->lodLevel ), vEntity->entityDef->globalReferenceBounds.GetCenter(), 0.2f, colorWhite, tr.viewDef->renderView.viewaxis );
		}
		tr.pc.c_mocIndexes += vEntity->mocIndexes;
		tr.pc.c_mocVerts += vEntity->mocVerts;
		tr.pc.c_mocTests += vEntity->mocTests;
		tr.pc.c_mocCulledSurfaces += vEntity->mocCulled;
		// RB
		if( vEntity->drawSurfs != NULL )
		{
			tr.pc.c_visibleViewEntities++;
		}

		for( drawSurf_t* ds = vEntity->drawSurfs; ds != NULL; )
		{
			drawSurf_t* next = ds->nextOnLight;
			if( ds->linkChain == NULL )
			{
				R_LinkDrawSurfToView( ds, tr.viewDef );
			}
			else
			{
				ds->nextOnLight = *ds->linkChain;
				*ds->linkChain = ds;
			}
			ds = next;
		}

		vEntity->drawSurfs = NULL;
	}
}
