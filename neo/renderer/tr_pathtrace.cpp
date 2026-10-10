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

  the world: when a map's first view is drawn, the triangles of the map's
  own models (its areas), the picture each of their materials is lit
  through, and the map's lights, are handed over once.

  what moves: every other model in the render world is an entity's, and the
  game may move, change or remove any of them, so none is handed over with
  the world. For every view the front end gathers the triangles of those in
  the areas that the eye's area is joined to by open portals, as they stand
  then: monsters and the weapon after they have been posed, doors and lifts
  where they are. What is behind a shut door is neither seen nor lights
  anything that is.

  the lights: every frame each light the world was handed with is given the
  colour it has now, which is none if it has gone or moved, and every light
  that was not there then, or is somewhere else now, is handed over afresh.

  a view: the eye is handed over, the tracer draws into a picture that it
  shares with OpenGL, and that picture is drawn where the 3D view would have
  been. Everything 2D is then drawn over it by OpenGL as it always was.

What is not there yet: translucent surfaces, particles, surfaces the game
bends as it draws them, and all that the lights' own pictures do.

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
idCVar r_ptAutoExposure( "r_ptAutoExposure", "0", CVAR_RENDERER | CVAR_BOOL | CVAR_ARCHIVE, "path tracer: adapt the exposure to the scene" );
idCVar r_ptLightScale( "r_ptLightScale", "2", CVAR_RENDERER | CVAR_FLOAT | CVAR_ARCHIVE, "path tracer: how bright a light is for its colour and size" );
idCVar r_ptDenoise( "r_ptDenoise", "4", CVAR_RENDERER | CVAR_INTEGER | CVAR_ARCHIVE, "path tracer: passes of the spatial filter, 0-4" );
idCVar r_ptEntities( "r_ptEntities", "1", CVAR_RENDERER | CVAR_BOOL, "path tracer: trace the models of entities: monsters, doors, the weapon and all else that is not the map itself" );
idCVar r_ptLightSamples( "r_ptLightSamples", "8", CVAR_RENDERER | CVAR_INTEGER | CVAR_ARCHIVE, "path tracer: lights weighed at each point before one is chosen, up to 64" );
idCVar r_ptFireflyClamp( "r_ptFireflyClamp", "16", CVAR_RENDERER | CVAR_FLOAT | CVAR_ARCHIVE, "path tracer: the brightest a single bounced sample may be" );
idCVar r_ptGlow( "r_ptGlow", "0.1", CVAR_RENDERER | CVAR_FLOAT | CVAR_ARCHIVE, "path tracer: how much light a surface the game draws unlit gives off, for a white texel; it is shown as painted whatever this is; needs reloadPathTrace" );
idCVar r_ptView( "r_ptView", "0", CVAR_RENDERER | CVAR_INTEGER, "path tracer: draw the scene some other way: 1 clay, 4 light alone, 7 base colour, 8 normals" );

// OpenGL's side of opening the tracer's picture (GL_EXT_memory_object, GL_EXT_memory_object_win32)
#define PT_GL_HANDLE_TYPE_OPAQUE_WIN32_EXT	0x9587
#define PT_GL_RGBA8							0x8058
#define PT_GL_CLAMP_TO_EDGE					0x812F

typedef void ( APIENTRY *ptCreateMemoryObjects_t )( GLsizei n, GLuint *memoryObjects );
typedef void ( APIENTRY *ptDeleteMemoryObjects_t )( GLsizei n, const GLuint *memoryObjects );
typedef void ( APIENTRY *ptImportMemoryWin32Handle_t )( GLuint memory, unsigned __int64 size, GLenum handleType, void *handle );
typedef void ( APIENTRY *ptTexStorageMem2D_t )( GLenum target, GLsizei levels, GLenum internalFormat, GLsizei width, GLsizei height, GLuint memory, unsigned __int64 offset );

// the most lights that are handed over each frame: every one is weighed at every point lit
const int PT_MAX_FRAME_LIGHTS = 32;

// a light the world was handed with
typedef struct {
	int					defIndex;			// which of the render world's lights
	pt_point_light_t	light;				// where it was, and how bright it was given as
	float				level;				// its intensity is this times what a light of colour 1 has
} ptWorldLight_t;

// the triangles of one entity among those gathered
typedef struct {
	int		defIndex;
	int		first, count;		// triangles
} ptSpan_t;

// a material of something that moves
typedef struct {
	const idMaterial *	shader;
	unsigned int		flags;		// PT_MAT_NO_SHADOW, PT_MAT_HELD
} ptMoving_t;

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
	idList<ptWorldLight_t>	worldLights;		// its lights, as they were then
	idList<float>			lightScales;		// 3 for each of them: what to multiply it by this frame
	idList<pt_point_light_t>	frameLights;	// and the lights it was not handed with
	idStr					mapName;
	ID_TIME_T				mapTimeStamp;
	bool					restart;
	bool					listLights;			// listPathTraceLights: say what the next view's are
	bool					listEntities;		// listPathTraceEntities: the same of its entities

	// what moves, gathered by the front end for one view
	const viewDef_t *		gatheredFor;
	idList<float>			positions;			// 9 per triangle
	idList<float>			uvs;				// 6
	idList<float>			normals;			// 9
	idList<float>			prevPositions;		// 9: where it was in the view before
	idList<unsigned int>	triMaterials;		// an index into moving
	idList<ptSpan_t>		spans, prevSpans;	// whose triangles are where
	idList<float>			lastPositions;		// the view before's positions
	idList<ptMoving_t>		moving;				// the materials of what moves
	idList<pt_material_t>	movingMaterials;	// the same, as the tracer has them
	idList<const idImage *>	movingImages;		// the pictures they are lit through
	idList<int>				movingTextures;		// and the tracer's handle for each, or -1
	int						gatherMsec;
} pt;

// what is gathered of a world before it is handed over
typedef struct {
	idList<pt_texture_t>		textures;
	idList<byte *>				pixels;			// to free
	idList<const idImage *>		images;			// of each texture
	idList<pt_material_t>		materials;
	idList<const idMaterial *>	shaders;		// of each material
	idList<bool>				noShadows;		// and whether it is for a model that casts no shadow
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
	pt.worldLights.Clear();
	pt.lightScales.Clear();
	pt.frameLights.Clear();
	pt.positions.Clear();
	pt.uvs.Clear();
	pt.normals.Clear();
	pt.prevPositions.Clear();
	pt.triMaterials.Clear();
	pt.spans.Clear();
	pt.prevSpans.Clear();
	pt.lastPositions.Clear();
	pt.moving.Clear();
	pt.movingMaterials.Clear();
	pt.movingImages.Clear();
	pt.movingTextures.Clear();
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
PT_ReadStages

What the tracer takes from a material: the picture it is lit through, or
failing that one it shows unlit, and whether that picture's alpha cuts holes
in it. The game cuts holes by any stage it likes, and a monster's are cut by
a picture of its own that burns it away when it dies: only holes cut by the
picture that is shown can be given to the tracer.
================
*/
static const idImage *PT_ReadStages( const idMaterial *shader, bool &glows, bool &holes ) {
	const shaderStage_t	*diffuse = NULL;
	const shaderStage_t	*unlit = NULL;

	for ( int i = 0; i < shader->GetNumStages(); i++ ) {
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
	const shaderStage_t *shown = diffuse ? diffuse : unlit;
	glows = !diffuse && unlit;
	holes = shown && shown->hasAlphaTest;
	return shown ? shown->texture.image : NULL;
}

/*
================
PT_MaterialFor

Returns the index in the build's materials, or -1 for a surface that is not traced.
================
*/
static int PT_MaterialFor( ptWorldBuild_t &build, const idMaterial *shader, bool noShadow ) {
	int					i;
	bool				glows, holes;
	pt_material_t		material;

	if ( !shader || !shader->IsDrawn() || shader->Coverage() == MC_TRANSLUCENT ) {
		return -1;
	}
	// The game's lamps sit inside their fittings, which it tells not to cast
	// shadows. Here that has to be said of the material.
	if ( !shader->SurfaceCastsShadow() ) {
		noShadow = true;
	}
	for ( i = 0; i < build.shaders.Num(); i++ ) {
		if ( build.shaders[i] == shader && build.noShadows[i] == noShadow ) {
			return i;
		}
	}

	const idImage *image = PT_ReadStages( shader, glows, holes );

	memset( &material, 0, sizeof( material ) );
	material.texture = PT_TextureFor( build, image );
	material.normal_texture = -1;
	material.anim_next = -1;
	material.alpha = 1.0f;
	material.roughness = 0.7f;
	if ( holes ) {
		material.flags |= PT_MAT_ALPHA_TEST;
	}
	if ( noShadow ) {
		material.flags |= PT_MAT_NO_SHADOW;
	}
	if ( glows ) {
		// not lit by the game's lights: it shows as it is painted, so it glows
		material.flags |= PT_MAT_EMIT_TEXTURE;
		material.emission[0] = material.emission[1] = material.emission[2] = r_ptGlow.GetFloat(); material.emission_seen = 1.0f;
	}

	build.shaders.Append( shader );
	build.noShadows.Append( noShadow );
	return build.materials.Append( material );
}

/*
================
PT_IsOfTheMap

The map's own models, which nothing moves, are handed over with the world.
Every other model is gathered for each view.
================
*/
static bool PT_IsOfTheMap( const idRenderEntityLocal *def ) {
	const idRenderModel *model = def->parms.hModel;
	return model && model->IsDynamicModel() == DM_STATIC && !def->parms.callback && idStr::Cmpn( model->Name(), "_area", 5 ) == 0;
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
		int material = PT_MaterialFor( build, shader, def->parms.noShadow );
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
PT_LightFor

A light of the game is a box or a pyramid that it fills, brightest in the
middle and dark at the edge, and nowhere brighter than its colour. The
tracer's is a point whose light thins with the square of the distance. It is
told to reach as far as the game's does and no further, and to be no brighter
nearer than half way out than it is there, where it is given the brightness
that lights a surface facing it as the game would.

Fills in the light as one of colour 1, and the colour it has in this view.
Returns false for what is not a light to the tracer, or not in this view.
================
*/
static bool PT_LightFor( const idRenderLightLocal *def, const viewDef_t *viewDef, pt_point_light_t &light, idVec3 &colour ) {
	const renderLight_t	&parms = def->parms;
	const idMaterial	*shader = def->lightShader;
	float				reach;
	int					i;

	if ( !shader || shader->IsFogLight() || shader->IsBlendLight() || shader->IsAmbientLight() ) {
		return false;
	}
	// the player's own, which only the player sees, and the other way round
	if ( parms.allowLightInViewID && parms.allowLightInViewID != viewDef->renderView.viewID ) {
		return false;
	}
	if ( parms.suppressLightInViewID && parms.suppressLightInViewID == viewDef->renderView.viewID ) {
		return false;
	}

	memset( &light, 0, sizeof( light ) );
	if ( parms.pointLight ) {
		reach = Max( parms.lightRadius.x, Max( parms.lightRadius.y, parms.lightRadius.z ) );
		light.origin[0] = def->globalLightOrigin.x;
		light.origin[1] = def->globalLightOrigin.y;
		light.origin[2] = def->globalLightOrigin.z;
	} else {
		idVec3	dir = parms.target * parms.axis;
		float	length = dir.Normalize();
		float	side = Max( parms.right.Length(), parms.up.Length() );

		if ( length <= 0.0f ) {
			return false;
		}
		reach = parms.end.LengthSqr() > 0.0f ? parms.end.Length() : length;
		light.origin[0] = parms.origin.x;
		light.origin[1] = parms.origin.y;
		light.origin[2] = parms.origin.z;
		light.direction[0] = dir.x;
		light.direction[1] = dir.y;
		light.direction[2] = dir.z;
		light.cone_cos = length / idMath::Sqrt( length * length + side * side );
	}
	if ( reach <= 0.0f ) {
		return false;
	}
	light.reach = reach;
	light.core = reach * 0.5f;
	light.intensity[0] = light.intensity[1] = light.intensity[2] = idMath::PI * light.core * light.core;

	// its colour is what its material makes of the colour it was given: that is
	// where a light flickers, pulses and fades
	float *regs = (float *) _alloca( shader->GetNumRegisters() * sizeof( float ) );
	shader->EvaluateRegisters( regs, parms.shaderParms, viewDef, parms.referenceSound );
	colour.Zero();
	for ( i = 0; i < shader->GetNumStages(); i++ ) {
		const shaderStage_t *stage = shader->GetStage( i );
		if ( !regs[ stage->conditionRegister ] ) {
			continue;
		}
		colour.x += Max( 0.0f, regs[ stage->color.registers[0] ] );
		colour.y += Max( 0.0f, regs[ stage->color.registers[1] ] );
		colour.z += Max( 0.0f, regs[ stage->color.registers[2] ] );
	}
	return true;
}

static float PT_Level( const idVec3 &colour ) {
	return colour.x * 0.2126f + colour.y * 0.7152f + colour.z * 0.0722f;
}

/*
================
PT_AddLight

A light the world is handed with. It is given as white, and as bright as it
is now but never as dark: one that is off may come on, and the tracer looks
for a light as often as the brightness it was handed with earns it.
================
*/
static void PT_AddLight( ptWorldBuild_t &build, int defIndex, const idRenderLightLocal *def, const viewDef_t *viewDef ) {
	ptWorldLight_t	worldLight;
	idVec3			colour;

	if ( !PT_LightFor( def, viewDef, worldLight.light, colour ) ) {
		return;
	}
	worldLight.defIndex = defIndex;
	worldLight.level = Max( PT_Level( colour ), 0.25f );
	for ( int i = 0; i < 3; i++ ) {
		worldLight.light.intensity[i] *= worldLight.level;
	}
	pt.worldLights.Append( worldLight );
	build.lights.Append( worldLight.light );
}

/*
================
PT_FrameLights

What the world's lights are now, and the lights it was not handed with.
================
*/
typedef struct {
	pt_point_light_t	light;
	float				away;		// how far the eye is from where its light ends
} ptFrameLight_t;

static int PT_NearestFirst( const void *a, const void *b ) {
	const float d = ( (const ptFrameLight_t *) a )->away - ( (const ptFrameLight_t *) b )->away;
	return d < 0.0f ? -1 : ( d > 0.0f ? 1 : 0 );
}

static void PT_FrameLights( const viewDef_t *viewDef ) {
	const idRenderWorldLocal	*world = viewDef->renderWorld;
	const float					scale = r_ptLightScale.GetFloat();
	const int					numDefs = world->lightDefs.Num();
	pt_point_light_t			light;
	idVec3						colour;
	int							i, j;

	bool *known = (bool *) _alloca( numDefs + 1 );
	memset( known, 0, numDefs + 1 );

	pt.lightScales.SetNum( pt.worldLights.Num() * 3, false );
	for ( i = 0; i < pt.worldLights.Num(); i++ ) {
		const ptWorldLight_t	&was = pt.worldLights[i];
		float					*out = &pt.lightScales[i * 3];

		out[0] = out[1] = out[2] = 0.0f;
		if ( was.defIndex >= numDefs || !world->lightDefs[was.defIndex] ) {
			continue;		// gone
		}
		if ( !PT_LightFor( world->lightDefs[was.defIndex], viewDef, light, colour ) ) {
			known[was.defIndex] = true;
			continue;
		}
		float moved = idMath::Fabs( light.reach - was.light.reach ) + idMath::Fabs( light.core - was.light.core ) + idMath::Fabs( light.cone_cos - was.light.cone_cos ) * 100.0f;
		for ( j = 0; j < 3; j++ ) {
			moved += idMath::Fabs( light.origin[j] - was.light.origin[j] );
			moved += idMath::Fabs( light.direction[j] - was.light.direction[j] ) * 100.0f;
		}
		if ( moved > 0.5f ) {
			continue;		// it is somewhere else now, and one of the frame's
		}
		known[was.defIndex] = true;
		out[0] = colour.x * scale / was.level;
		out[1] = colour.y * scale / was.level;
		out[2] = colour.z * scale / was.level;
	}

	idList<ptFrameLight_t>	found;
	for ( i = 0; i < numDefs; i++ ) {
		ptFrameLight_t	frameLight;

		if ( known[i] || !world->lightDefs[i] ) {
			continue;
		}
		if ( !PT_LightFor( world->lightDefs[i], viewDef, frameLight.light, colour ) || PT_Level( colour ) <= 0.0f ) {
			continue;
		}
		for ( j = 0; j < 3; j++ ) {
			frameLight.light.intensity[j] *= colour[j] * scale;
		}
		const idVec3 origin( frameLight.light.origin[0], frameLight.light.origin[1], frameLight.light.origin[2] );
		frameLight.away = ( origin - viewDef->renderView.vieworg ).Length() - frameLight.light.reach;
		found.Append( frameLight );
	}
	if ( found.Num() > PT_MAX_FRAME_LIGHTS ) {
		qsort( found.Ptr(), found.Num(), sizeof( ptFrameLight_t ), PT_NearestFirst );
		found.SetNum( PT_MAX_FRAME_LIGHTS, false );
	}
	pt.frameLights.SetNum( found.Num(), false );
	for ( i = 0; i < found.Num(); i++ ) {
		pt.frameLights[i] = found[i].light;
	}

	if ( pt.listLights ) {
		const idVec3	eye = viewDef->renderView.vieworg;
		int				lit = 0, left = 0;

		pt.listLights = false;
		for ( i = 0; i < numDefs; i++ ) {
			const idRenderLightLocal *def = world->lightDefs[i];
			if ( !def ) {
				continue;
			}
			const idMaterial	*shader = def->lightShader;
			const char			*kind = "";
			if ( !PT_LightFor( def, viewDef, light, colour ) ) {
				kind = shader && shader->IsAmbientLight() ? "ambient, left out" : ( shader && shader->IsFogLight() ? "fog, left out" :
						( shader && shader->IsBlendLight() ? "blend, left out" : "left out" ) );
				left++;
			} else {
				kind = known[i] ? "the map's" : "the frame's";
				if ( PT_Level( colour ) > 0.0f ) {
					lit++;
				}
			}
			const float reach = def->parms.pointLight ? Max( def->parms.lightRadius.x, Max( def->parms.lightRadius.y, def->parms.lightRadius.z ) ) : def->parms.target.Length();
			if ( ( def->globalLightOrigin - eye ).Length() > reach ) {
				continue;
			}
			common->Printf( "%4i %-18s %s colour %.2f %.2f %.2f reach %.0f, %.0f away%s %s\n", i, kind, def->parms.pointLight ? "point" : "cone ",
							colour.x, colour.y, colour.z, reach, ( def->globalLightOrigin - eye ).Length(), def->parms.noShadows ? " noshadows" : "", shader ? shader->GetName() : "" );
		}
		common->Printf( "path tracer: %d lights in the world: %d the map's, %d the frame's (of %d found), %d lit, %d left out\n",
						world->lightDefs.Num(), pt.worldLights.Num(), pt.frameLights.Num(), found.Num(), lit, left );
	}
}

/*
================
PT_LoadWorld
================
*/
static void PT_LoadWorld( const viewDef_t *viewDef ) {
	const idRenderWorldLocal *world = viewDef->renderWorld;
	ptWorldBuild_t	build;
	pt_world_t		w;
	int				i;
	int				start = Sys_Milliseconds();

	build.positions.SetGranularity( 65536 );
	build.uvs.SetGranularity( 65536 );
	build.indices.SetGranularity( 65536 );
	build.triMaterials.SetGranularity( 65536 );

	for ( i = 0; i < world->entityDefs.Num(); i++ ) {
		const idRenderEntityLocal *def = world->entityDefs[i];

		if ( !def || !PT_IsOfTheMap( def ) ) {
			continue;
		}
		PT_AddModel( build, def );
	}
	pt.worldLights.Clear();
	for ( i = 0; i < world->lightDefs.Num(); i++ ) {
		if ( world->lightDefs[i] ) {
			PT_AddLight( build, i, world->lightDefs[i], viewDef );
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

	const int gathered = Sys_Milliseconds();
	pt.backend->load_world( pt.backend, &w );

	for ( i = 0; i < build.pixels.Num(); i++ ) {
		R_StaticFree( build.pixels[i] );
	}

	pt.world = world;
	pt.mapName = world->mapName;
	pt.mapTimeStamp = world->mapTimeStamp;
	pt.restart = true;

	common->Printf( "path tracer: %s: %d triangles, %d materials, %d textures, %d lights, %d msec to gather and %d to hand over\n", world->mapName.c_str(),
					w.num_triangles, w.num_materials, w.num_textures, w.num_lights, gathered - start, Sys_Milliseconds() - gathered );
	common->Printf( "path tracer: %d triangles of %d entities gathered in %d msec\n", pt.triMaterials.Num(), pt.spans.Num(), pt.gatherMsec );
}

/*
================
PT_MovingFor

The place among the materials of what moves of this one, or -1 for a
surface that is not traced. The front end asks; the tracer is given the
material when the view is drawn.
================
*/
static int PT_MovingFor( const idMaterial *shader, unsigned int flags ) {
	int i;

	if ( !shader || !shader->IsDrawn() || shader->Coverage() == MC_TRANSLUCENT || shader->Deform() != DFRM_NONE ) {
		return -1;
	}
	if ( !shader->SurfaceCastsShadow() ) {
		flags |= PT_MAT_NO_SHADOW;
	}
	for ( i = 0; i < pt.moving.Num(); i++ ) {
		if ( pt.moving[i].shader == shader && pt.moving[i].flags == flags ) {
			return i;
		}
	}
	ptMoving_t moving;
	moving.shader = shader;
	moving.flags = flags;
	return pt.moving.Append( moving );
}

// makes a list longer without making room for it a little at a time
template< class type >
static void PT_Grow( idList<type> &list, int num ) {
	if ( num > list.NumAllocated() ) {
		list.Resize( num + num / 2 + 4096 );
	}
	list.SetNum( num, false );
}

/*
================
PT_GatherView

The front end's part: the triangles of every entity's model, as they stand
for the view that is about to be handed to the back end.
================
*/
void PT_GatherView( void ) {
	viewDef_t			*viewDef = tr.viewDef;
	idRenderWorldLocal	*world;
	int					i, j, k;

	if ( !viewDef || viewDef->isSubview || !r_pathTrace.GetBool() || !viewDef->renderWorld ) {
		return;
	}
	world = viewDef->renderWorld;
	if ( world->mapName.Length() == 0 ) {
		return;
	}

	const int start = Sys_Milliseconds();

	// where everything was in the view before
	pt.lastPositions.SetNum( pt.positions.Num(), false );
	if ( pt.positions.Num() ) {
		memcpy( pt.lastPositions.Ptr(), pt.positions.Ptr(), pt.positions.Num() * sizeof( float ) );
	}
	pt.prevSpans.SetNum( pt.spans.Num(), false );
	if ( pt.spans.Num() ) {
		memcpy( pt.prevSpans.Ptr(), pt.spans.Ptr(), pt.spans.Num() * sizeof( ptSpan_t ) );
	}
	int *spanOf = (int *) _alloca( ( world->entityDefs.Num() + 1 ) * sizeof( int ) );
	for ( i = 0; i < world->entityDefs.Num(); i++ ) {
		spanOf[i] = -1;
	}
	for ( i = 0; i < pt.prevSpans.Num(); i++ ) {
		if ( pt.prevSpans[i].defIndex < world->entityDefs.Num() ) {
			spanOf[pt.prevSpans[i].defIndex] = i;
		}
	}

	pt.positions.SetNum( 0, false );
	pt.uvs.SetNum( 0, false );
	pt.normals.SetNum( 0, false );
	pt.prevPositions.SetNum( 0, false );
	pt.triMaterials.SetNum( 0, false );
	pt.spans.SetNum( 0, false );
	pt.gatheredFor = viewDef;

	if ( !r_ptEntities.GetBool() ) {
		return;
	}

	const int	viewID = viewDef->renderView.viewID;
	const bool	list = pt.listEntities;
	pt.listEntities = false;
	for ( i = 0; i < world->entityDefs.Num(); i++ ) {
		idRenderEntityLocal *def = world->entityDefs[i];

		if ( !def || !def->parms.hModel || PT_IsOfTheMap( def ) ) {
			continue;
		}
		// only what is in an area the eye's is joined to
		if ( viewDef->connectedAreas ) {
			const areaReference_t *ref;
			for ( ref = def->entityRefs; ref; ref = ref->ownerNext ) {
				if ( viewDef->connectedAreas[ ref->area->areaNum ] ) {
					break;
				}
			}
			if ( !ref ) {
				continue;
			}
		}
		// the player's own body, which the player does not see, and what only the player sees
		if ( def->parms.suppressSurfaceInViewID && def->parms.suppressSurfaceInViewID == viewID ) {
			continue;
		}
		if ( def->parms.allowSurfaceInViewID && def->parms.allowSurfaceInViewID != viewID ) {
			continue;
		}

		// posed, if it is one that is: the game is asked as it would be were it in view
		const idRenderModel *model = R_EntityDefDynamicModel( def );
		if ( !model ) {
			continue;
		}

		unsigned int flags = 0;
		if ( def->parms.noShadow ) {
			flags |= PT_MAT_NO_SHADOW;
		}
		if ( def->parms.weaponDepthHack ) {
			flags |= PT_MAT_HELD;
		}

		int numModelTris = 0;
		ptSpan_t span;
		span.defIndex = i;
		span.first = pt.triMaterials.Num();

		const idVec3 &origin = def->parms.origin;
		const idMat3 &axis = def->parms.axis;
		for ( j = 0; j < model->NumSurfaces(); j++ ) {
			const modelSurface_t	*surf = model->Surface( j );
			srfTriangles_t			*tri = surf->geometry;

			if ( !tri || tri->numVerts <= 0 || tri->numIndexes < 3 || !tri->verts || !tri->indexes ) {
				continue;
			}
			numModelTris += tri->numIndexes / 3;
			const idMaterial *shader = R_RemapShaderBySkin( surf->shader, def->parms.customSkin, def->parms.customShader );
			const int material = PT_MovingFor( shader, flags );
			if ( material < 0 ) {
				if ( list && ( origin - viewDef->renderView.vieworg ).Length() < 512.0f ) {
					common->Printf( "      left out: %s\n", shader ? shader->GetName() : "no material" );
				}
				continue;
			}
			if ( !tri->tangentsCalculated ) {
				R_DeriveTangents( tri );		// a posed model's normals are worked out when they are first wanted
			}

			const int numTris = tri->numIndexes / 3;
			int at = pt.triMaterials.Num();
			PT_Grow( pt.triMaterials, at + numTris );
			PT_Grow( pt.positions, ( at + numTris ) * 9 );
			PT_Grow( pt.normals, ( at + numTris ) * 9 );
			PT_Grow( pt.uvs, ( at + numTris ) * 6 );
			for ( k = 0; k < numTris; k++, at++ ) {
				float *p = &pt.positions[at * 9], *n = &pt.normals[at * 9], *uv = &pt.uvs[at * 6];

				pt.triMaterials[at] = material;
				// the engine's triangles face the other way round from the tracer's
				for ( int c = 0; c < 3; c++ ) {
					const idDrawVert	&v = tri->verts[ tri->indexes[k * 3 + 2 - c] ];
					const idVec3		xyz = origin + v.xyz * axis;
					const idVec3		normal = v.normal * axis;

					p[c * 3] = xyz.x;		p[c * 3 + 1] = xyz.y;		p[c * 3 + 2] = xyz.z;
					n[c * 3] = normal.x;	n[c * 3 + 1] = normal.y;	n[c * 3 + 2] = normal.z;
					uv[c * 2] = v.st.x;		uv[c * 2 + 1] = v.st.y;
				}
			}
		}

		span.count = pt.triMaterials.Num() - span.first;
		if ( span.count > 0 ) {
			pt.spans.Append( span );
		}
		if ( list && ( origin - viewDef->renderView.vieworg ).Length() < 512.0f ) {
			common->Printf( "%5i %6i of %6i tris  %s%s\n", i, span.count, numModelTris, def->parms.hModel->Name(), ( flags & PT_MAT_HELD ) ? " (held)" : "" );
		}
	}

	// where each triangle was in the view before, if the same entity had as many then
	pt.prevPositions.SetNum( pt.positions.Num(), false );
	for ( i = 0; i < pt.spans.Num(); i++ ) {
		const ptSpan_t	&span = pt.spans[i];
		const int		was = spanOf[span.defIndex];
		const float		*from = &pt.positions[span.first * 9];

		if ( was >= 0 && pt.prevSpans[was].count == span.count && ( pt.prevSpans[was].first + span.count ) * 9 <= pt.lastPositions.Num() ) {
			from = &pt.lastPositions[pt.prevSpans[was].first * 9];
		}
		memcpy( &pt.prevPositions[span.first * 9], from, span.count * 9 * sizeof( float ) );
	}

	pt.gatherMsec = Sys_Milliseconds() - start;
}

/*
================
PT_MovingMaterials

The back end's part of the same: the tracer's material for each material of
what moves that it has not yet been given.
================
*/
static void PT_MovingMaterials( void ) {
	int i, j;

	for ( i = pt.movingMaterials.Num(); i < pt.moving.Num(); i++ ) {
		const idMaterial	*shader = pt.moving[i].shader;
		bool				glows, holes;
		pt_material_t		material;
		const idImage		*image = PT_ReadStages( shader, glows, holes );

		memset( &material, 0, sizeof( material ) );
		material.texture = -1;
		material.normal_texture = -1;
		material.anim_next = -1;
		material.alpha = 1.0f;
		material.roughness = 0.7f;
		material.flags = pt.moving[i].flags;
		if ( holes ) {
			material.flags |= PT_MAT_ALPHA_TEST;
		}
		if ( image ) {
			for ( j = 0; j < pt.movingImages.Num(); j++ ) {
				if ( pt.movingImages[j] == image ) {
					break;
				}
			}
			if ( j == pt.movingImages.Num() ) {
				byte	*pic = NULL;
				int		width = 0, height = 0, handle = -1;

				R_LoadImageProgram( image->imgName, &pic, &width, &height, NULL );
				if ( pic && width > 0 && height > 0 ) {
					pt_texture_t texture;
					texture.width = width;
					texture.height = height;
					texture.pixels = (const uint32_t *) pic;
					handle = pt.backend->texture_create( pt.backend, &texture );
				}
				if ( pic ) {
					R_StaticFree( pic );
				}
				pt.movingImages.Append( image );
				pt.movingTextures.Append( handle );
			}
			material.texture = pt.movingTextures[j];
		}
		if ( glows ) {
			// not lit by the game's lights: it shows as it is painted, so it glows
			material.flags |= PT_MAT_EMIT_TEXTURE;
			material.emission[0] = material.emission[1] = material.emission[2] = r_ptGlow.GetFloat(); material.emission_seen = 1.0f;
		}
		pt.movingMaterials.Append( material );
	}
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
PT_ListEntities_f
================
*/
void PT_ListEntities_f( const idCmdArgs &args ) {
	pt.listEntities = true;
}

/*
================
PT_ListLights_f
================
*/
void PT_ListLights_f( const idCmdArgs &args ) {
	pt.listLights = true;
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
	pt_scene_t		scene;

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
		PT_LoadWorld( viewDef );
	}
	PT_FrameLights( viewDef );

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

	memset( &scene, 0, sizeof( scene ) );
	scene.lights = pt.frameLights.Ptr();
	scene.num_lights = pt.frameLights.Num();
	if ( pt.gatheredFor == viewDef && pt.triMaterials.Num() > 0 ) {
		PT_MovingMaterials();
		scene.materials = pt.movingMaterials.Ptr();
		scene.num_materials = pt.movingMaterials.Num();
		scene.positions = pt.positions.Ptr();
		scene.uvs = pt.uvs.Ptr();
		scene.normals = pt.normals.Ptr();
		scene.prev_positions = pt.prevPositions.Ptr();
		scene.tri_materials = pt.triMaterials.Ptr();
		scene.num_triangles = pt.triMaterials.Num();
	}
	view.scene = &scene;
	view.light_scales = pt.lightScales.Ptr();
	view.num_light_scales = pt.worldLights.Num();

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
	view.light_samples = r_ptLightSamples.GetInteger();
	view.firefly_clamp = r_ptFireflyClamp.GetFloat();
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
