/*
===========================================================================

Doom 3 GPL Source Code
Copyright (C) 1999-2011 id Software LLC, a ZeniMax Media company.

This file is part of the Doom 3 GPL Source Code ("Doom 3 Source Code").

Doom 3 Source Code is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

Doom 3 Source Code is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with Doom 3 Source Code.  If not, see <http://www.gnu.org/licenses/>.

In addition, the Doom 3 Source Code is also subject to certain additional terms. You should have received a copy of these additional terms immediately following the terms and conditions of the GNU General Public License which accompanied the Doom 3 Source Code.  If not, please request a copy in writing from id Software at the address below.

If you have questions concerning this license or the applicable additional terms, you may contact in writing id Software LLC, c/o ZeniMax Media Inc., Suite 120, Rockville, Maryland 20850 USA.

===========================================================================
*/

/*

The path traced view (r_pathTrace 1).

The tracer is the library in pt/, which knows nothing of this engine. This
file is what stands between the two:

  the world: when a map's first view is drawn, the triangles of every model
  in the render world that does not deform, the picture each of their
  materials is lit through, and the map's lights, are handed over once.

  a view: the eye is handed over, the tracer draws into a picture that it
  shares with OpenGL, and that picture is drawn where the 3D view would have
  been. Everything 2D is then drawn over it by OpenGL as it always was.

What is not there yet: anything that moves or deforms (doors stay where
they were when the map was handed over), translucent surfaces, lights that
change, and all that the lights' own pictures do.

*/

#include "../idlib/precompiled.h"
#pragma hdrstop

#include "tr_local.h"

#include "../../pt/include/pt.h"

extern "C" pt_backend_t *pt_rtx_create( const pt_create_t *ci, char *err, int errlen );

idCVar r_pathTrace( "r_pathTrace", "0", CVAR_RENDERER | CVAR_BOOL | CVAR_ARCHIVE, "draw 3D views with the path tracer" );
idCVar r_ptScale( "r_ptScale", "0.5", CVAR_RENDERER | CVAR_FLOAT | CVAR_ARCHIVE, "path tracer: resolution traced at, as a fraction of the view" );
idCVar r_ptSamples( "r_ptSamples", "1", CVAR_RENDERER | CVAR_INTEGER | CVAR_ARCHIVE, "path tracer: paths per pixel per frame" );
idCVar r_ptBounces( "r_ptBounces", "3", CVAR_RENDERER | CVAR_INTEGER | CVAR_ARCHIVE, "path tracer: bounces after the first surface" );
idCVar r_ptExposure( "r_ptExposure", "1", CVAR_RENDERER | CVAR_FLOAT | CVAR_ARCHIVE, "path tracer: exposure" );
idCVar r_ptAutoExposure( "r_ptAutoExposure", "1", CVAR_RENDERER | CVAR_BOOL | CVAR_ARCHIVE, "path tracer: adapt the exposure to the scene" );
idCVar r_ptLightScale( "r_ptLightScale", "0.4", CVAR_RENDERER | CVAR_FLOAT | CVAR_ARCHIVE, "path tracer: how bright a light is for its colour and size; needs reloadPathTrace" );
idCVar r_ptDenoise( "r_ptDenoise", "4", CVAR_RENDERER | CVAR_INTEGER | CVAR_ARCHIVE, "path tracer: passes of the spatial filter, 0-4" );
idCVar r_ptView( "r_ptView", "0", CVAR_RENDERER | CVAR_INTEGER, "path tracer: draw the scene some other way: 1 clay, 4 light alone, 7 base colour, 8 normals" );

// OpenGL's side of opening the tracer's picture (GL_EXT_memory_object, GL_EXT_memory_object_win32)
#define PT_GL_HANDLE_TYPE_OPAQUE_WIN32_EXT	0x9587
#define PT_GL_RGBA8							0x8058
#define PT_GL_CLAMP_TO_EDGE					0x812F

typedef void ( APIENTRY *ptCreateMemoryObjects_t )( GLsizei n, GLuint *memoryObjects );
typedef void ( APIENTRY *ptDeleteMemoryObjects_t )( GLsizei n, const GLuint *memoryObjects );
typedef void ( APIENTRY *ptImportMemoryWin32Handle_t )( GLuint memory, unsigned __int64 size, GLenum handleType, void *handle );
typedef void ( APIENTRY *ptTexStorageMem2D_t )( GLenum target, GLsizei levels, GLenum internalFormat, GLsizei width, GLsizei height, GLuint memory, unsigned __int64 offset );

static struct {
	pt_backend_t *			backend;
	bool					failed;				// do not try again until the renderer restarts
	int						width, height;

	GLuint					glMemory;
	GLuint					glTexture;
	ptDeleteMemoryObjects_t	deleteMemoryObjects;

	idList<unsigned int>	overlay;			// nothing: the 2D is OpenGL's
	bool					overlaySent;

	const idRenderWorldLocal *world;			// the one that was handed over
	idStr					mapName;
	ID_TIME_T				mapTimeStamp;
	bool					restart;
} pt;

// what is gathered of a world before it is handed over
typedef struct {
	idList<pt_texture_t>		textures;
	idList<byte *>				pixels;			// to free
	idList<const idImage *>		images;			// of each texture
	idList<pt_material_t>		materials;
	idList<const idMaterial *>	shaders;		// of each material
	idList<float>				positions;
	idList<float>				uvs;
	idList<unsigned int>		indices;
	idList<unsigned int>		triMaterials;
	idList<pt_point_light_t>	lights;
} ptWorldBuild_t;

static void PT_Log( const char *msg ) {
	common->Printf( "%s", msg );
}

/*
================
PT_Shutdown

Called when OpenGL is shut down, and whenever the tracer has to be made again.
================
*/
void PT_Shutdown( void ) {
	if ( pt.glTexture ) {
		qglDeleteTextures( 1, &pt.glTexture );
	}
	if ( pt.glMemory && pt.deleteMemoryObjects ) {
		pt.deleteMemoryObjects( 1, &pt.glMemory );
	}
	if ( pt.backend ) {
		pt.backend->destroy( pt.backend );
	}
	pt.overlay.Clear();
	pt.mapName.Clear();
	memset( &pt, 0, sizeof( pt ) );
}

/*
================
PT_Start

Makes the tracer and opens its picture as an OpenGL texture.
================
*/
static bool PT_Start( void ) {
	pt_create_t			ci;
	pt_shared_image_t	shared;
	char				err[1024];

	if ( pt.backend ) {
		if ( pt.width == glConfig.vidWidth && pt.height == glConfig.vidHeight ) {
			return true;
		}
		PT_Shutdown();
	}
	if ( pt.failed ) {
		return false;
	}

	ptCreateMemoryObjects_t		createMemoryObjects = (ptCreateMemoryObjects_t) GLimp_ExtensionPointer( "glCreateMemoryObjectsEXT" );
	ptImportMemoryWin32Handle_t	importMemory = (ptImportMemoryWin32Handle_t) GLimp_ExtensionPointer( "glImportMemoryWin32HandleEXT" );
	ptTexStorageMem2D_t			texStorageMem2D = (ptTexStorageMem2D_t) GLimp_ExtensionPointer( "glTexStorageMem2DEXT" );
	pt.deleteMemoryObjects = (ptDeleteMemoryObjects_t) GLimp_ExtensionPointer( "glDeleteMemoryObjectsEXT" );
	if ( !createMemoryObjects || !importMemory || !texStorageMem2D || !pt.deleteMemoryObjects ) {
		common->Printf( "path tracer: this OpenGL driver cannot open another API's picture (GL_EXT_memory_object_win32)\n" );
		pt.failed = true;
		return false;
	}

	memset( &ci, 0, sizeof( ci ) );
	ci.width = glConfig.vidWidth;
	ci.height = glConfig.vidHeight;
	ci.log = PT_Log;
	ci.shared = 1;
	err[0] = '\0';
	pt.backend = pt_rtx_create( &ci, err, sizeof( err ) );
	if ( !pt.backend ) {
		common->Printf( "path tracer: %s\n", err );
		pt.failed = true;
		return false;
	}
	if ( !pt.backend->shared_image || !pt.backend->shared_image( pt.backend, &shared ) ) {
		common->Printf( "path tracer: no shared picture\n" );
		PT_Shutdown();
		pt.failed = true;
		return false;
	}
	pt.width = shared.width;
	pt.height = shared.height;

	createMemoryObjects( 1, &pt.glMemory );
	importMemory( pt.glMemory, shared.size, PT_GL_HANDLE_TYPE_OPAQUE_WIN32_EXT, shared.handle );

	globalImages->BindNull();
	qglGenTextures( 1, &pt.glTexture );
	qglBindTexture( GL_TEXTURE_2D, pt.glTexture );
	qglTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR );
	qglTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
	qglTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, PT_GL_CLAMP_TO_EDGE );
	qglTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, PT_GL_CLAMP_TO_EDGE );
	texStorageMem2D( GL_TEXTURE_2D, 1, PT_GL_RGBA8, pt.width, pt.height, pt.glMemory, 0 );
	qglBindTexture( GL_TEXTURE_2D, 0 );
	backEnd.glState.tmu[backEnd.glState.currenttmu].current2DMap = -1;

	if ( qglGetError() != GL_NO_ERROR ) {
		common->Printf( "path tracer: OpenGL could not open the shared picture\n" );
		PT_Shutdown();
		pt.failed = true;
		return false;
	}

	pt.overlay.SetNum( pt.width * pt.height );
	memset( pt.overlay.Ptr(), 0, pt.overlay.Num() * sizeof( unsigned int ) );
	pt.overlaySent = false;

	common->Printf( "path tracer: %s on %s, %d x %d\n", pt.backend->name, pt.backend->device, pt.width, pt.height );
	return true;
}

/*
================
PT_TextureFor

The pixels of an image, read from its file again: the renderer keeps none.
Returns the index in the build's textures, or -1 for none.
================
*/
static int PT_TextureFor( ptWorldBuild_t &build, const idImage *image ) {
	int		i;
	byte	*pic = NULL;
	int		width = 0, height = 0;

	if ( !image ) {
		return -1;
	}
	for ( i = 0; i < build.images.Num(); i++ ) {
		if ( build.images[i] == image ) {
			return i;
		}
	}
	R_LoadImageProgram( image->imgName, &pic, &width, &height, NULL );
	if ( !pic || width <= 0 || height <= 0 ) {
		return -1;
	}

	pt_texture_t texture;
	texture.width = width;
	texture.height = height;
	texture.pixels = (const uint32_t *) pic;
	build.pixels.Append( pic );
	build.images.Append( image );
	return build.textures.Append( texture );
}

/*
================
PT_MaterialFor

Returns the index in the build's materials, or -1 for a surface that is not traced.
================
*/
static int PT_MaterialFor( ptWorldBuild_t &build, const idMaterial *shader ) {
	int					i;
	const shaderStage_t	*diffuse = NULL;
	const shaderStage_t	*unlit = NULL;
	pt_material_t		material;

	if ( !shader || !shader->IsDrawn() || shader->Coverage() == MC_TRANSLUCENT ) {
		return -1;
	}
	for ( i = 0; i < build.shaders.Num(); i++ ) {
		if ( build.shaders[i] == shader ) {
			return i;
		}
	}

	for ( i = 0; i < shader->GetNumStages(); i++ ) {
		const shaderStage_t *stage = shader->GetStage( i );
		if ( !stage->texture.image || stage->texture.cinematic || stage->texture.dynamic != DI_STATIC ) {
			continue;
		}
		if ( stage->lighting == SL_DIFFUSE && !diffuse ) {
			diffuse = stage;
		} else if ( stage->lighting == SL_AMBIENT && !unlit ) {
			unlit = stage;
		}
	}

	memset( &material, 0, sizeof( material ) );
	material.texture = -1;
	material.normal_texture = -1;
	material.anim_next = -1;
	material.alpha = 1.0f;
	material.roughness = 0.7f;
	if ( shader->Coverage() == MC_PERFORATED ) {
		material.flags |= PT_MAT_ALPHA_TEST;
	}
	if ( diffuse ) {
		material.texture = PT_TextureFor( build, diffuse->texture.image );
	} else if ( unlit ) {
		// not lit by the game's lights: it shows as it is painted, so it glows
		material.texture = PT_TextureFor( build, unlit->texture.image );
		material.flags |= PT_MAT_EMIT_TEXTURE;
		material.emission[0] = material.emission[1] = material.emission[2] = 0.5f;
	}

	build.shaders.Append( shader );
	return build.materials.Append( material );
}

/*
================
PT_AddModel
================
*/
static void PT_AddModel( ptWorldBuild_t &build, const idRenderEntityLocal *def ) {
	const idRenderModel	*model = def->parms.hModel;
	int					i, j;

	for ( i = 0; i < model->NumSurfaces(); i++ ) {
		const modelSurface_t	*surf = model->Surface( i );
		const srfTriangles_t	*tri = surf->geometry;

		if ( !tri || tri->numVerts <= 0 || tri->numIndexes < 3 || !tri->verts || !tri->indexes ) {
			continue;
		}
		const idMaterial *shader = R_RemapShaderBySkin( surf->shader, def->parms.customSkin, def->parms.customShader );
		int material = PT_MaterialFor( build, shader );
		if ( material < 0 ) {
			continue;
		}

		const unsigned int first = build.positions.Num() / 3;
		for ( j = 0; j < tri->numVerts; j++ ) {
			const idVec3 p = def->parms.origin + tri->verts[j].xyz * def->parms.axis;
			build.positions.Append( p.x );
			build.positions.Append( p.y );
			build.positions.Append( p.z );
			build.uvs.Append( tri->verts[j].st.x );
			build.uvs.Append( tri->verts[j].st.y );
		}
		for ( j = 0; j + 2 < tri->numIndexes; j += 3 ) {
			// the engine's triangles face the other way round from the tracer's
			build.indices.Append( first + tri->indexes[j + 2] );
			build.indices.Append( first + tri->indexes[j + 1] );
			build.indices.Append( first + tri->indexes[j] );
			build.triMaterials.Append( material );
		}
	}
}

/*
================
PT_AddLight

A light of the game is a box or a pyramid that it fills, brightest in the
middle and dark at the edge. The tracer's is a point whose light thins with
the square of the distance. It is given the brightness that puts about the
same light half way out.
================
*/
static void PT_AddLight( ptWorldBuild_t &build, const idRenderLightLocal *def ) {
	const renderLight_t	&parms = def->parms;
	pt_point_light_t	light;
	float				reach;

	if ( !def->lightShader || def->lightShader->IsFogLight() || def->lightShader->IsBlendLight() || def->lightShader->IsAmbientLight() ) {
		return;
	}

	memset( &light, 0, sizeof( light ) );
	if ( parms.pointLight ) {
		reach = ( parms.lightRadius.x + parms.lightRadius.y + parms.lightRadius.z ) * ( 1.0f / 3.0f );
		light.origin[0] = def->globalLightOrigin.x;
		light.origin[1] = def->globalLightOrigin.y;
		light.origin[2] = def->globalLightOrigin.z;
	} else {
		idVec3	dir = parms.target * parms.axis;
		float	length = dir.Normalize();
		float	side = Max( parms.right.Length(), parms.up.Length() );

		if ( length <= 0.0f ) {
			return;
		}
		reach = length;
		light.origin[0] = parms.origin.x;
		light.origin[1] = parms.origin.y;
		light.origin[2] = parms.origin.z;
		light.direction[0] = dir.x;
		light.direction[1] = dir.y;
		light.direction[2] = dir.z;
		light.cone_cos = length / idMath::Sqrt( length * length + side * side );
	}
	if ( reach <= 0.0f ) {
		return;
	}

	const float scale = r_ptLightScale.GetFloat() * reach * reach;
	light.intensity[0] = parms.shaderParms[SHADERPARM_RED] * scale;
	light.intensity[1] = parms.shaderParms[SHADERPARM_GREEN] * scale;
	light.intensity[2] = parms.shaderParms[SHADERPARM_BLUE] * scale;
	if ( light.intensity[0] + light.intensity[1] + light.intensity[2] <= 0.0f ) {
		return;
	}
	build.lights.Append( light );
}

/*
================
PT_LoadWorld
================
*/
static void PT_LoadWorld( const idRenderWorldLocal *world ) {
	ptWorldBuild_t	build;
	pt_world_t		w;
	int				i;
	int				start = Sys_Milliseconds();

	for ( i = 0; i < world->entityDefs.Num(); i++ ) {
		const idRenderEntityLocal *def = world->entityDefs[i];

		if ( !def || !def->parms.hModel || def->parms.hModel->IsDynamicModel() != DM_STATIC ) {
			continue;
		}
		// what is held in the hand, and what only shows in a mirror
		if ( def->parms.weaponDepthHack || def->parms.allowSurfaceInViewID ) {
			continue;
		}
		PT_AddModel( build, def );
	}
	for ( i = 0; i < world->lightDefs.Num(); i++ ) {
		if ( world->lightDefs[i] ) {
			PT_AddLight( build, world->lightDefs[i] );
		}
	}

	memset( &w, 0, sizeof( w ) );
	w.textures = build.textures.Ptr();
	w.num_textures = build.textures.Num();
	w.materials = build.materials.Ptr();
	w.num_materials = build.materials.Num();
	w.positions = build.positions.Ptr();
	w.uvs = build.uvs.Ptr();
	w.num_vertices = build.positions.Num() / 3;
	w.indices = build.indices.Ptr();
	w.tri_materials = build.triMaterials.Ptr();
	w.num_triangles = build.triMaterials.Num();
	w.lights = build.lights.Ptr();
	w.num_lights = build.lights.Num();
	for ( i = 0; i < 6; i++ ) {
		w.sky_textures[i] = -1;
	}
	w.sky_scale = 1.0f;

	pt.backend->load_world( pt.backend, &w );

	for ( i = 0; i < build.pixels.Num(); i++ ) {
		R_StaticFree( build.pixels[i] );
	}

	pt.world = world;
	pt.mapName = world->mapName;
	pt.mapTimeStamp = world->mapTimeStamp;
	pt.restart = true;

	common->Printf( "path tracer: %s: %d triangles, %d materials, %d textures, %d lights, %d msec\n", world->mapName.c_str(),
					w.num_triangles, w.num_materials, w.num_textures, w.num_lights, Sys_Milliseconds() - start );
}

/*
================
PT_Reload_f
================
*/
void PT_Reload_f( const idCmdArgs &args ) {
	pt.world = NULL;
}

/*
================
PT_DrawView

Draws the back end's current view with the tracer. Returns false if it could
not, and the view is then OpenGL's to draw.
================
*/
bool PT_DrawView( void ) {
	const viewDef_t	*viewDef = backEnd.viewDef;
	pt_view_t		view;

	if ( !r_pathTrace.GetBool() || !viewDef->renderWorld || !viewDef->viewEntitys || viewDef->isSubview ) {
		return false;
	}
	if ( viewDef->renderWorld->mapName.Length() == 0 ) {
		return false;		// not a map: the player's model in a menu, say
	}
	if ( !PT_Start() ) {
		return false;
	}
	if ( pt.world != viewDef->renderWorld || pt.mapTimeStamp != viewDef->renderWorld->mapTimeStamp
			|| pt.mapName != viewDef->renderWorld->mapName ) {
		PT_LoadWorld( viewDef->renderWorld );
	}

	const renderView_t &rv = viewDef->renderView;
	const int width = viewDef->viewport.x2 - viewDef->viewport.x1 + 1;
	const int height = viewDef->viewport.y2 - viewDef->viewport.y1 + 1;

	memset( &view, 0, sizeof( view ) );
	view.x = viewDef->viewport.x1;
	view.y = pt.height - 1 - viewDef->viewport.y2;		// the tracer counts rows from the top
	view.width = width;
	view.height = height;
	view.time = rv.time * 0.001f;
	for ( int i = 0; i < 3; i++ ) {
		view.origin[i] = rv.vieworg[i];
		view.forward[i] = rv.viewaxis[0][i];
		view.right[i] = -rv.viewaxis[1][i];		// the engine's second axis points left
		view.up[i] = rv.viewaxis[2][i];
	}
	view.fov_x = rv.fov_x;
	view.fov_y = rv.fov_y;
	view.restart = pt.restart;
	pt.restart = false;

	view.scale = idMath::ClampFloat( 0.25f, 1.0f, r_ptScale.GetFloat() );
	view.samples = Max( 1, r_ptSamples.GetInteger() );
	view.bounces = Max( 0, r_ptBounces.GetInteger() );
	view.exposure = r_ptExposure.GetFloat();
	view.auto_exposure = r_ptAutoExposure.GetBool();
	view.antialias = 1;
	view.filter = 2;
	view.view_mode = r_ptView.GetInteger();
	view.reflections = 2;
	view.reflection_rate = 1.0f;
	view.light_samples = 8;
	view.firefly_clamp = 16.0f;
	view.texture_filter = 1;
	view.denoise = r_ptDenoise.GetInteger();
	view.history = 8;
	view.reflection_history = 1;
	view.saturation = 1.0f;
	view.contrast = 1.0f;
	if ( PT_VIEW_IS_MEASURE( view.view_mode ) ) {
		view.exposure = 1.0f;
		view.auto_exposure = 0;
		view.tonemap = 2;
	}
	if ( r_ptView.IsModified() ) {
		r_ptView.ClearModified();
		view.restart = 1;
	}

	pt.backend->render_view( pt.backend, &view );
	pt.backend->present( pt.backend, pt.overlay.Ptr(), NULL, pt.overlaySent ? 0 : -1 );
	pt.overlaySent = true;

	// the picture, over the whole window: outside the view it is black
	qglViewport( 0, 0, glConfig.vidWidth, glConfig.vidHeight );
	if ( r_useScissor.GetBool() ) {
		qglScissor( 0, 0, glConfig.vidWidth, glConfig.vidHeight );
	}
	qglMatrixMode( GL_PROJECTION );
	qglPushMatrix();
	qglLoadIdentity();
	qglOrtho( 0, 1, 0, 1, -1, 1 );
	qglMatrixMode( GL_MODELVIEW );
	qglPushMatrix();
	qglLoadIdentity();

	GL_State( GLS_DEPTHFUNC_ALWAYS | GLS_DEPTHMASK );
	GL_Cull( CT_TWO_SIDED );
	GL_SelectTexture( 0 );
	globalImages->BindNull();
	qglBindTexture( GL_TEXTURE_2D, pt.glTexture );
	qglEnable( GL_TEXTURE_2D );
	qglColor4f( 1, 1, 1, 1 );

	// the picture's first row is its top one
	qglBegin( GL_QUADS );
	qglTexCoord2f( 0, 1 ); qglVertex2f( 0, 0 );
	qglTexCoord2f( 1, 1 ); qglVertex2f( 1, 0 );
	qglTexCoord2f( 1, 0 ); qglVertex2f( 1, 1 );
	qglTexCoord2f( 0, 0 ); qglVertex2f( 0, 1 );
	qglEnd();

	qglDisable( GL_TEXTURE_2D );
	qglBindTexture( GL_TEXTURE_2D, 0 );
	backEnd.glState.tmu[0].current2DMap = -1;

	qglMatrixMode( GL_PROJECTION );
	qglPopMatrix();
	qglMatrixMode( GL_MODELVIEW );
	qglPopMatrix();

	return true;
}
