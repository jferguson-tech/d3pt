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

  materials: the game lights a surface through three pictures, its colour,
  its bumps and the colour it shines in, and the tracer is given the same
  three. See PT_ReadMaterial.

  glass: of all that the game draws see-through, glass alone is traced: a
  clear pane with a faint tint, which reflects, and bends what is seen
  through it. See PT_IsGlass.

  what OpenGL still draws: the screens of the game's computers, particles,
  decals, flares and everything else see-through but glass are not light
  falling on surfaces, they are pictures the game lays over the scene, each
  blended in a way of its own. They are drawn by OpenGL as they always
  were, over the traced picture, hidden where the scene hides them. They do
  not show in what the tracer reflects and they light nothing. See
  PT_DrawOverlay.

What is not there yet: other translucent surfaces, particles, surfaces the game
bends as it draws them, and all that the lights' own pictures do.

*/

#include "../idlib/precompiled.h"
#pragma hdrstop

#include "tr_local.h"

#include "../../pt/include/pt.h"

extern "C" pt_backend_t *pt_rtx_create( const pt_create_t *ci, char *err, int errlen );

int RB_STD_DrawShaderPasses( drawSurf_t **drawSurfs, int numDrawSurfs );

idCVar r_pathTrace( "r_pathTrace", "0", CVAR_RENDERER | CVAR_BOOL | CVAR_ARCHIVE, "draw 3D views with the path tracer" );
idCVar r_ptPreset( "r_ptPreset", "0", CVAR_RENDERER | CVAR_INTEGER, "path tracer: 1 low, 2 medium, 3 high, 4 ultra: set, it sets r_ptScale, r_ptSamples, r_ptBounces and r_ptLightSamples; it reads 0 when they are none of the four" );
idCVar r_ptScale( "r_ptScale", "1", CVAR_RENDERER | CVAR_FLOAT | CVAR_ARCHIVE, "path tracer: resolution traced at, as a fraction of the view" );
idCVar r_ptSamples( "r_ptSamples", "1", CVAR_RENDERER | CVAR_INTEGER | CVAR_ARCHIVE, "path tracer: paths per pixel per frame" );
idCVar r_ptBounces( "r_ptBounces", "3", CVAR_RENDERER | CVAR_INTEGER | CVAR_ARCHIVE, "path tracer: bounces after the first surface" );
idCVar r_ptExposure( "r_ptExposure", "1", CVAR_RENDERER | CVAR_FLOAT | CVAR_ARCHIVE, "path tracer: exposure" );
idCVar r_ptAutoExposure( "r_ptAutoExposure", "0", CVAR_RENDERER | CVAR_BOOL | CVAR_ARCHIVE, "path tracer: adapt the exposure to the scene" );
idCVar r_ptLightScale( "r_ptLightScale", "1", CVAR_RENDERER | CVAR_FLOAT | CVAR_ARCHIVE, "path tracer: how bright a light is for its colour and size" );
idCVar r_ptDenoise( "r_ptDenoise", "4", CVAR_RENDERER | CVAR_INTEGER | CVAR_ARCHIVE, "path tracer: passes of the spatial filter, 0-4" );
idCVar r_ptEntities( "r_ptEntities", "1", CVAR_RENDERER | CVAR_BOOL, "path tracer: trace the models of entities: monsters, doors, the weapon and all else that is not the map itself" );
idCVar r_ptLightSamples( "r_ptLightSamples", "8", CVAR_RENDERER | CVAR_INTEGER | CVAR_ARCHIVE, "path tracer: lights weighed at each point before one is chosen, up to 64" );
idCVar r_ptFireflyClamp( "r_ptFireflyClamp", "16", CVAR_RENDERER | CVAR_FLOAT | CVAR_ARCHIVE, "path tracer: the brightest a single bounced sample may be" );
idCVar r_ptGlow( "r_ptGlow", "0.1", CVAR_RENDERER | CVAR_FLOAT | CVAR_ARCHIVE, "path tracer: how much light a surface the game draws unlit gives off, for a white texel; it is shown as painted whatever this is; needs reloadPathTrace" );
idCVar r_ptShadowFaces( "r_ptShadowFaces", "1", CVAR_RENDERER | CVAR_BOOL | CVAR_ARCHIVE, "path tracer: shadows are cast by the side of a surface that is turned to the light, as the game casts them: a lamp set inside a wall shines out of it" );
idCVar r_ptMaterials( "r_ptMaterials", "1", CVAR_RENDERER | CVAR_BOOL | CVAR_ARCHIVE, "path tracer: read the bump and specular maps of materials; 0: their colour alone, all of them matte; needs reloadPathTrace" );
idCVar r_ptSpecular( "r_ptSpecular", "1.5", CVAR_RENDERER | CVAR_FLOAT | CVAR_ARCHIVE, "path tracer: how much of the light a surface reflects, for a white texel of its specular map; needs reloadPathTrace" );
idCVar r_ptGloss( "r_ptGloss", "0.5", CVAR_RENDERER | CVAR_FLOAT | CVAR_ARCHIVE, "path tracer: how rough a surface with a specular map is, 0.05 a mirror to 1; it sets how wide a highlight is; needs reloadPathTrace" );
idCVar r_ptSky( "r_ptSky", "0.5", CVAR_RENDERER | CVAR_FLOAT | CVAR_ARCHIVE, "path tracer: how bright the sky is, for a white texel of its pictures; it lights what it shines on; needs reloadPathTrace" );
idCVar r_ptAntialias( "r_ptAntialias", "0", CVAR_RENDERER | CVAR_BOOL | CVAR_ARCHIVE, "path tracer: blend frames over time to smooth edges; it also softens the whole picture" );
idCVar r_ptFilter( "r_ptFilter", "2", CVAR_RENDERER | CVAR_INTEGER | CVAR_ARCHIVE, "path tracer: what is done about noise: 2 frames are blended over time and the picture filtered, 1 neither but frames add up at rest, 0 neither" );
idCVar r_ptHistory( "r_ptHistory", "8", CVAR_RENDERER | CVAR_INTEGER | CVAR_ARCHIVE, "path tracer: frames of lighting kept while things change" );
idCVar r_ptOverlay( "r_ptOverlay", "1", CVAR_RENDERER | CVAR_BOOL | CVAR_ARCHIVE, "path tracer: OpenGL draws over the traced view what the tracer leaves out: computer screens, particles, decals, flares" );
idCVar r_ptTonemap( "r_ptTonemap", "0", CVAR_RENDERER | CVAR_INTEGER | CVAR_ARCHIVE, "path tracer: how light is turned into a picture: 0 filmic, 1 neutral, 2 clipped as the game clips it" );
idCVar r_ptContrast( "r_ptContrast", "1", CVAR_RENDERER | CVAR_FLOAT | CVAR_ARCHIVE, "path tracer: contrast of the picture, 1 = unchanged" );
idCVar r_ptSaturation( "r_ptSaturation", "1", CVAR_RENDERER | CVAR_FLOAT | CVAR_ARCHIVE, "path tracer: saturation of the picture, 1 = unchanged" );
idCVar r_ptGlass( "r_ptGlass", "1", CVAR_RENDERER | CVAR_BOOL | CVAR_ARCHIVE, "path tracer: trace glass; needs reloadPathTrace" );
idCVar r_ptGlassTint( "r_ptGlassTint", "0.08", CVAR_RENDERER | CVAR_FLOAT | CVAR_ARCHIVE, "path tracer: how much of what is behind a pane of glass its own colour covers, 0 to 1; needs reloadPathTrace" );
idCVar r_ptRefraction( "r_ptRefraction", "1", CVAR_RENDERER | CVAR_BOOL | CVAR_ARCHIVE, "path tracer: glass bends what is seen through it" );
idCVar r_ptShowTimes( "r_ptShowTimes", "0", CVAR_RENDERER | CVAR_INTEGER, "path tracer: print where the time of a frame goes, averaged over this many frames" );
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
	bool					forgetMoving;		// reloadPathTrace: the materials of what moves are to be read again

	// what moves, gathered by the front end for one view
	const viewDef_t *		gatheredFor;
	idList<float>			positions;			// 9 per triangle
	idList<float>			uvs;				// 6
	idList<float>			normals;			// 9
	idList<float>			tangents;			// 18: at each corner the way u runs, then the way v does
	idList<float>			prevPositions;		// 9: where it was in the view before
	idList<unsigned int>	triMaterials;		// an index into moving
	idList<ptSpan_t>		spans, prevSpans;	// whose triangles are where
	idList<float>			lastPositions;		// the view before's positions
	idList<ptMoving_t>		moving;				// the materials of what moves
	idList<pt_material_t>	movingMaterials;	// the same, as the tracer has them
	idList<const idMaterial *>	movingRead;		// the game's materials whose pictures the tracer has
	idList<int>				movingBase;			// and its handles for the two it was given for each, or -1
	idList<int>				movingDetail;
	idList<int>				movingShine;
	int						gatherMsec;
	double					gatherMs;				// the same, finer
} pt;

// what is gathered of a world before it is handed over
typedef struct {
	idList<pt_texture_t>		textures;
	idList<byte *>				pixels;			// to free
	idList<const idMaterial *>	read;			// the game's materials whose pictures are among the textures
	idList<int>					readBase;		// and which two are each one's, or -1
	idList<int>					readDetail;
	idList<int>					readShine;
	idList<pt_material_t>		materials;
	idList<const idMaterial *>	shaders;		// of each material
	idList<bool>				noShadows;		// and whether it is for a model that casts no shadow
	idList<float>				positions;
	idList<float>				uvs;
	idList<float>				normals;		// 3 a vertex
	idList<float>				tangents;		// 6 a vertex
	idList<unsigned int>		indices;
	idList<unsigned int>		triMaterials;
	idList<pt_point_light_t>	lights;
	const idImage *				sky;			// the six pictures of the first sky met with, or NULL
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
	pt.tangents.Clear();
	pt.prevPositions.Clear();
	pt.triMaterials.Clear();
	pt.spans.Clear();
	pt.prevSpans.Clear();
	pt.lastPositions.Clear();
	pt.moving.Clear();
	pt.movingMaterials.Clear();
	pt.movingRead.Clear();
	pt.movingBase.Clear();
	pt.movingDetail.Clear();
	pt.movingShine.Clear();
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
PT_IsGlass

The game draws many things see-through: glass, but also decals, smoke,
glows and flares, each by blending some picture over what is behind in a
way of its own. Only glass is something a path tracer has a thing for. A
material says it is glass for the sake of the sound and the marks a bullet
makes on it, and that is what is gone by, or failing that its name.
================
*/
static bool PT_IsGlass( const idMaterial *shader ) {
	if ( !r_ptGlass.GetBool() || shader->Coverage() != MC_TRANSLUCENT || shader->Deform() != DFRM_NONE ) {
		return false;
	}
	return shader->GetSurfaceType() == SURFTYPE_GLASS || idStr::FindText( shader->GetName(), "glass", false ) >= 0;
}

/*
================
PT_IsTraced
================
*/
static bool PT_IsTraced( const idMaterial *shader ) {
	if ( !shader || !shader->IsDrawn() ) {
		return false;
	}
	return shader->Coverage() != MC_TRANSLUCENT || PT_IsGlass( shader );
}

/*
================
PT_IsMirror

Whether a material is a mirror. The game draws one by drawing the scene a
second time from behind it; the tracer has only to be told that the surface
reflects everything and scatters nothing.
================
*/
static bool PT_IsMirror( const idMaterial *shader ) {
	bool other = false;

	for ( int i = 0; i < shader->GetNumStages(); i++ ) {
		const dynamicidImage_t dynamic = shader->GetStage( i )->texture.dynamic;
		if ( dynamic == DI_MIRROR_RENDER ) {
			return true;
		}
		if ( dynamic != DI_STATIC ) {
			other = true;		// a camera's picture, or the like
		}
	}
	// (the keyword "mirror" leaves no more trace than this)
	return shader->GetSort() == SS_SUBVIEW && !other;
}

/*
================
PT_SkyOf

The six pictures a material shows the sky by, if that is what it does: the
game draws such a surface with a picture looked up by the way the eye looks
through it, not by where on the surface it is.
================
*/
static const idImage *PT_SkyOf( const idMaterial *shader ) {
	for ( int i = 0; i < shader->GetNumStages(); i++ ) {
		const shaderStage_t *stage = shader->GetStage( i );
		if ( stage->texture.image && ( stage->texture.texgen == TG_SKYBOX_CUBE || stage->texture.texgen == TG_WOBBLESKY_CUBE ) ) {
			return stage->texture.image;
		}
	}
	return NULL;
}

/*
================
PT_SkyFaces

The sky's six pictures as the tracer wants them, each size by size and to be
freed with R_StaticFree. The game's are the six faces of an OpenGL cube map
whose axes are the world's. The tracer has its own way of laying a
direction on a face (see pt_world_t's sky_textures), so each of its texels
is looked up in the game's by its direction.
================
*/
static bool PT_SkyFaces( const idImage *image, byte *faces[6], int &size ) {
	byte *pics[6];
	int i;

	memset( pics, 0, sizeof( pics ) );
	if ( !R_LoadCubeImages( image->imgName, image->cubeFiles, pics, &size, NULL ) || size <= 0 ) {
		return false;
	}
	for ( int face = 0; face < 6; face++ ) {
		const int a = face / 2, b = ( a + 1 ) % 3, c = ( a + 2 ) % 3;

		faces[face] = (byte *) R_StaticAlloc( size * size * 4 );
		for ( int y = 0; y < size; y++ ) {
			for ( int x = 0; x < size; x++ ) {
				float d[3];
				d[a] = ( face & 1 ) ? -1.0f : 1.0f;
				d[b] = 2.0f * ( x + 0.5f ) / size - 1.0f;
				d[c] = 2.0f * ( y + 0.5f ) / size - 1.0f;

				// OpenGL's choice of face and of where on it
				const float ax = idMath::Fabs( d[0] ), ay = idMath::Fabs( d[1] ), az = idMath::Fabs( d[2] );
				int		of;
				float	sc, tc, ma;
				if ( ax >= ay && ax >= az ) {
					of = d[0] > 0.0f ? 0 : 1;	ma = ax;	sc = d[0] > 0.0f ? -d[2] : d[2];	tc = -d[1];
				} else if ( ay >= az ) {
					of = d[1] > 0.0f ? 2 : 3;	ma = ay;	sc = d[0];							tc = d[1] > 0.0f ? d[2] : -d[2];
				} else {
					of = d[2] > 0.0f ? 4 : 5;	ma = az;	sc = d[2] > 0.0f ? d[0] : -d[0];	tc = -d[1];
				}
				const int tx = idMath::ClampInt( 0, size - 1, (int) ( ( sc / ma + 1.0f ) * 0.5f * size ) );
				const int ty = idMath::ClampInt( 0, size - 1, (int) ( ( tc / ma + 1.0f ) * 0.5f * size ) );
				memcpy( &faces[face][( y * size + x ) * 4], &pics[of][( ty * size + tx ) * 4], 4 );
			}
		}
	}
	for ( i = 0; i < 6; i++ ) {
		if ( pics[i] ) {
			R_StaticFree( pics[i] );
		}
	}
	return true;
}

// what is read of one of the game's materials for the tracer
typedef struct {
	byte	*base;			// its colour, and in alpha its holes; NULL for plain white
	int		baseWidth, baseHeight;
	byte	*detail;		// its normal, and in alpha how rough it is; NULL for none
	int		detailWidth, detailHeight;
	byte	*shine;			// the colour it reflects head on; NULL for none
	int		shineWidth, shineHeight;
	bool	glows;			// it is shown unlit
	bool	screen;			// one of the game's computer screens: OpenGL draws what is on it
	bool	mirror;			// a mirror: nothing else is read
	const idImage *sky;		// it shows the sky: the six pictures of it, and nothing else is read
	bool	holes;			// base's alpha cuts it out
	bool	glass;			// a clear pane: base is its tint
	bool	filter;			// glass whose picture the game multiplies what is behind by
} ptRead_t;

/*
================
PT_ReadImage

The pixels of an image, read from its file again: the renderer keeps none.
To be freed with R_StaticFree.
================
*/
static byte *PT_ReadImage( const idImage *image, int &width, int &height ) {
	byte *pic = NULL;

	width = height = 0;
	if ( !image ) {
		return NULL;
	}
	R_LoadImageProgram( image->imgName, &pic, &width, &height, NULL );
	if ( pic && ( width <= 0 || height <= 0 ) ) {
		R_StaticFree( pic );
		pic = NULL;
	}
	return pic;
}

/*
================
PT_ReadMaterial

The game lights a surface through a picture of its colour (diffuse), one of
its bumps (a normal map) and one of the colour it shines back at a light in
(specular): what is seen is the colour times the light, plus twice the
specular colour times a highlight of one fixed width.

The tracer is given the same three. The specular picture is what the
surface reflects head on, and one roughness for every surface that has one
stands for the game's one highlight: r_ptGloss, which with r_ptSpecular was
set by eye, against the game's own picture, so that a light makes a
highlight about as wide and as bright as the game's. (The specular picture
is taken to be painted for the screen, as the colour is, which makes it
darker than its numbers: hence a scale above 1.) What the tracer adds is that such a surface reflects the room as
well as the lights.

Holes: the game cuts them by any stage it likes, and a monster's are cut by
a picture of its own that burns it away when it dies. Only holes cut by the
picture that is shown can be given to the tracer.
================
*/
static void PT_ReadMaterial( const idMaterial *shader, ptRead_t &read, bool pictures ) {
	const shaderStage_t	*diffuse = NULL, *unlit = NULL, *bump = NULL, *specular = NULL;
	int					i;

	memset( &read, 0, sizeof( read ) );
	read.sky = PT_SkyOf( shader );
	if ( read.sky ) {
		return;
	}
	read.mirror = PT_IsMirror( shader );
	if ( read.mirror ) {
		return;
	}
	for ( i = 0; i < shader->GetNumStages(); i++ ) {
		const shaderStage_t *stage = shader->GetStage( i );
		if ( !stage->texture.image || stage->texture.cinematic || stage->texture.dynamic != DI_STATIC ) {
			continue;
		}
		if ( stage->texture.texgen != TG_EXPLICIT ) {
			continue;		// what a surface mirrors, as the game fakes it: the tracer has the real thing
		}
		if ( ( stage->drawStateBits & GLS_COLORMASK ) == GLS_COLORMASK ) {
			// It draws no colour, only alpha for a later stage to go by: on
			// glass, where the reflection the game fakes is to show. Taken
			// for the colour of the glass, every pane had the same picture
			// laid over what was behind it.
			continue;
		}
		if ( stage->lighting == SL_DIFFUSE && !diffuse ) {
			diffuse = stage;
		} else if ( stage->lighting == SL_AMBIENT && !unlit ) {
			unlit = stage;
		} else if ( stage->lighting == SL_BUMP && !bump ) {
			bump = stage;
		} else if ( stage->lighting == SL_SPECULAR && !specular ) {
			specular = stage;
		}
	}
	const shaderStage_t *shown = diffuse ? diffuse : unlit;
	read.glass = PT_IsGlass( shader );
	read.filter = read.glass && shown && ( shown->drawStateBits & ( GLS_SRCBLEND_BITS | GLS_DSTBLEND_BITS ) ) == ( GLS_SRCBLEND_DST_COLOR | GLS_DSTBLEND_ZERO );
	read.screen = shader->HasGui();
	read.glows = !diffuse && unlit && !read.glass;
	read.holes = shown && shown->hasAlphaTest && !read.glass;
	if ( !pictures ) {
		return;		// only what it is, for one whose pictures the tracer has
	}
	if ( shown ) {
		read.base = PT_ReadImage( shown->texture.image, read.baseWidth, read.baseHeight );
	}
	if ( ( !diffuse && !read.glass ) || !r_ptMaterials.GetBool() ) {
		return;
	}

	if ( specular && !read.glass ) {
		read.shine = PT_ReadImage( specular->texture.image, read.shineWidth, read.shineHeight );
	}
	if ( read.shine ) {
		const float scale = Max( 0.0f, r_ptSpecular.GetFloat() );
		const int count = read.shineWidth * read.shineHeight * 4;
		for ( i = 0; i < count; i++ ) {
			if ( ( i & 3 ) != 3 ) {
				read.shine[i] = (byte) Min( 255.0f, read.shine[i] * scale );
			}
		}
	}

	// the normal as it is, with how rough the surface is in its alpha
	if ( bump ) {
		read.detail = PT_ReadImage( bump->texture.image, read.detailWidth, read.detailHeight );
	}
	if ( !read.detail && read.shine ) {
		read.detailWidth = read.detailHeight = 4;
		read.detail = (byte *) R_StaticAlloc( 4 * 4 * 4 );
		for ( i = 0; i < 4 * 4; i++ ) {
			read.detail[i * 4] = read.detail[i * 4 + 1] = 128;
			read.detail[i * 4 + 2] = 255;
		}
	}
	if ( read.detail ) {
		// (glass is as smooth as the tracer has anything)
		const byte rough = (byte) ( idMath::ClampFloat( 0.05f, 1.0f, read.glass ? 0.05f : ( read.shine ? r_ptGloss.GetFloat() : 0.85f ) ) * 255.0f + 0.5f );
		const int count = read.detailWidth * read.detailHeight;
		for ( i = 0; i < count; i++ ) {
			read.detail[i * 4 + 3] = rough;
		}
	}
}

static void PT_FreeRead( ptRead_t &read ) {
	if ( read.base ) {
		R_StaticFree( read.base );
	}
	if ( read.detail ) {
		R_StaticFree( read.detail );
	}
	if ( read.shine ) {
		R_StaticFree( read.shine );
	}
}

/*
================
PT_FillMaterial

The tracer's material for one that was read, given where its pictures are.
================
*/
static void PT_FillMaterial( pt_material_t &material, const ptRead_t &read, int base, int detail, int shine, unsigned int flags ) {
	memset( &material, 0, sizeof( material ) );
	material.texture = base;
	material.normal_texture = detail;
	material.specular_texture = shine + 1;
	material.anim_next = -1;
	material.alpha = 1.0f;
	material.roughness = 0.85f;
	material.flags = flags;
	if ( read.glass ) {
		// (a pane with no colour of its own is clear: it shows by what it reflects)
		material.alpha = base >= 0 ? idMath::ClampFloat( 0.0f, 0.95f, r_ptGlassTint.GetFloat() ) : 0.0f;
		if ( read.filter && base >= 0 ) {
			material.alpha = 0.0f;
			material.flags |= PT_MAT_FILTER;
		}
		material.roughness = 0.05f;
		material.flags |= PT_MAT_REFRACT;
	}
	if ( read.holes ) {
		material.flags |= PT_MAT_ALPHA_TEST;
	}
	if ( read.sky ) {
		material.flags |= PT_MAT_SKY;
	}
	if ( read.screen ) {
		material.flags |= PT_MAT_BLACK;
	}
	if ( read.mirror ) {
		material.texture = -1;
		material.normal_texture = -1;
		material.specular_texture = 0;
		material.roughness = 0.0f;
		material.metallic = 1.0f;
		material.flags |= PT_MAT_MIRROR;
	}
	if ( read.glows ) {
		// not lit by the game's lights: it shows as it is painted, so it glows
		material.flags |= PT_MAT_EMIT_TEXTURE;
		material.emission[0] = material.emission[1] = material.emission[2] = r_ptGlow.GetFloat();
		material.emission_seen = 1.0f;
	}
}

/*
================
PT_MaterialFor

Returns the index in the build's materials, or -1 for a surface that is not traced.
================
*/
static int PT_MaterialFor( ptWorldBuild_t &build, const idMaterial *shader, bool noShadow ) {
	int				i;
	ptRead_t		read;
	pt_material_t	material;

	if ( !PT_IsTraced( shader ) ) {
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

	// its pictures may be there already, for the same material casting shadows or not
	for ( i = 0; i < build.read.Num(); i++ ) {
		if ( build.read[i] == shader ) {
			break;
		}
	}
	PT_ReadMaterial( shader, read, i == build.read.Num() );
	if ( read.sky && !build.sky ) {
		build.sky = read.sky;
	}
	if ( i == build.read.Num() ) {
		byte	*pixels[3] = { read.base, read.detail, read.shine };
		int		sizes[3][2] = { { read.baseWidth, read.baseHeight }, { read.detailWidth, read.detailHeight }, { read.shineWidth, read.shineHeight } };
		int		at[3] = { -1, -1, -1 };

		for ( int k = 0; k < 3; k++ ) {
			if ( pixels[k] ) {
				pt_texture_t texture;
				texture.width = sizes[k][0];
				texture.height = sizes[k][1];
				texture.pixels = (const uint32_t *) pixels[k];
				build.pixels.Append( pixels[k] );		// freed when the world has been handed over
				at[k] = build.textures.Append( texture );
			}
		}
		build.read.Append( shader );
		build.readBase.Append( at[0] );
		build.readDetail.Append( at[1] );
		build.readShine.Append( at[2] );
	}

	PT_FillMaterial( material, read, build.readBase[i], build.readDetail[i], build.readShine[i], noShadow ? PT_MAT_NO_SHADOW : 0 );

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
		srfTriangles_t			*tri = surf->geometry;

		if ( !tri || tri->numVerts <= 0 || tri->numIndexes < 3 || !tri->verts || !tri->indexes ) {
			continue;
		}
		const idMaterial *shader = R_RemapShaderBySkin( surf->shader, def->parms.customSkin, def->parms.customShader );
		int material = PT_MaterialFor( build, shader, def->parms.noShadow );
		if ( material < 0 ) {
			continue;
		}
		if ( !tri->tangentsCalculated ) {
			R_DeriveTangents( tri );		// (the game works them out when a surface is first lit)
		}

		// with each vertex its normal and its two tangents, as the game has them: its normal maps were made for these
		const unsigned int first = build.positions.Num() / 3;
		for ( j = 0; j < tri->numVerts; j++ ) {
			const idDrawVert	&v = tri->verts[j];
			const idVec3		p = def->parms.origin + v.xyz * def->parms.axis;
			const idVec3		along[3] = { v.normal * def->parms.axis, v.tangents[0] * def->parms.axis, v.tangents[1] * def->parms.axis };
			build.positions.Append( p.x );
			build.positions.Append( p.y );
			build.positions.Append( p.z );
			build.uvs.Append( v.st.x );
			build.uvs.Append( v.st.y );
			build.normals.Append( along[0].x );
			build.normals.Append( along[0].y );
			build.normals.Append( along[0].z );
			for ( int k = 1; k < 3; k++ ) {
				build.tangents.Append( along[k].x );
				build.tangents.Append( along[k].y );
				build.tangents.Append( along[k].z );
			}
		}
		const bool twoSided = shader->GetCullType() == CT_TWO_SIDED;
		for ( j = 0; j + 2 < tri->numIndexes; j += 3 ) {
			// the engine's triangles face the other way round from the tracer's
			build.indices.Append( first + tri->indexes[j + 2] );
			build.indices.Append( first + tri->indexes[j + 1] );
			build.indices.Append( first + tri->indexes[j] );
			build.triMaterials.Append( material );
			if ( twoSided ) {
				// a ray meets a triangle from its front only: one seen from both sides is there twice
				build.indices.Append( first + tri->indexes[j] );
				build.indices.Append( first + tri->indexes[j + 1] );
				build.indices.Append( first + tri->indexes[j + 2] );
				build.triMaterials.Append( material );
			}
		}
	}
}

/*
================
PT_LightFor

A light of the game is a box or a pyramid that it fills, brightest in the
middle and dark at the edge, and nowhere brighter than its colour, and its
light does not thin with distance. The place its light comes from, for
shading and for shadows, need not be the middle of the box, nor in it.

The tracer's is a point whose light thins with the square of the distance,
so it is told not to thin it at all as far as the light goes (core), and
given the brightness that lights a surface facing it as the game would. A
box is handed over as a box, which the tracer fades as the game does, from
its middle to its faces. A pyramid is a cone that fades at its far end.
A light the game casts no shadows from is one nothing stops.

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
		// the box, along the world's axes: one that is turned is taken as the box around it
		idVec3 size;
		for ( i = 0; i < 3; i++ ) {
			size[i] = idMath::Fabs( parms.axis[0][i] ) * parms.lightRadius.x + idMath::Fabs( parms.axis[1][i] ) * parms.lightRadius.y
					+ idMath::Fabs( parms.axis[2][i] ) * parms.lightRadius.z;
			light.origin[i] = def->globalLightOrigin[i];
			light.box_origin[i] = parms.origin[i];
			light.box_size[i] = size[i];
		}
		if ( size.x <= 0.0f || size.y <= 0.0f || size.z <= 0.0f ) {
			return false;
		}
		// as far as any of it is from where the light comes from
		reach = ( def->globalLightOrigin - parms.origin ).Length() + size.Length();
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
	light.core = reach;
	light.no_shadow = parms.noShadows || !shader->LightCastsShadows();
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

A light the world is handed with. It is given as white and as bright as
any other, whatever it is now: the tracer keeps for each part of the map
the lights that put most on it as they were handed over, and one that is
off or dim then may be the room's only light once it comes on.
================
*/
static void PT_AddLight( ptWorldBuild_t &build, int defIndex, const idRenderLightLocal *def, const viewDef_t *viewDef ) {
	ptWorldLight_t	worldLight;
	idVec3			colour;

	if ( !PT_LightFor( def, viewDef, worldLight.light, colour ) ) {
		return;
	}
	worldLight.defIndex = defIndex;
	worldLight.level = 1.0f;
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
		float moved = ( light.no_shadow != was.light.no_shadow ? 1.0f : 0.0f ) + idMath::Fabs( light.reach - was.light.reach ) + idMath::Fabs( light.core - was.light.core ) + idMath::Fabs( light.cone_cos - was.light.cone_cos ) * 100.0f;
		for ( j = 0; j < 3; j++ ) {
			moved += idMath::Fabs( light.origin[j] - was.light.origin[j] );
			moved += idMath::Fabs( light.box_origin[j] - was.light.box_origin[j] ) + idMath::Fabs( light.box_size[j] - was.light.box_size[j] );
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
			common->Printf( "%4i %-18s %s colour %.2f %.2f %.2f reach %.0f, %.0f away, from %.0f %.0f %.0f%s %s\n", i, kind, def->parms.pointLight ? "point" : "cone ",
							colour.x, colour.y, colour.z, reach, ( def->globalLightOrigin - eye ).Length(), def->globalLightOrigin.x, def->globalLightOrigin.y, def->globalLightOrigin.z, def->parms.noShadows ? " noshadows" : "", shader ? shader->GetName() : "" );
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

	build.sky = NULL;
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
	w.normals = build.normals.Ptr();
	w.tangents = build.tangents.Ptr();
	w.num_vertices = build.positions.Num() / 3;
	w.indices = build.indices.Ptr();
	w.tri_materials = build.triMaterials.Ptr();
	w.num_triangles = build.triMaterials.Num();
	w.lights = build.lights.Ptr();
	w.num_lights = build.lights.Num();
	for ( i = 0; i < 6; i++ ) {
		w.sky_textures[i] = -1;
	}
	w.sky_scale = Max( 0.0f, r_ptSky.GetFloat() );

	// the sky: one for the whole map, the first that any of its surfaces shows
	byte	*skyFaces[6];
	int		skySize = 0;
	if ( build.sky && PT_SkyFaces( build.sky, skyFaces, skySize ) ) {
		for ( i = 0; i < 6; i++ ) {
			pt_texture_t texture;
			texture.width = texture.height = skySize;
			texture.pixels = (const uint32_t *) skyFaces[i];
			build.pixels.Append( skyFaces[i] );
			w.sky_textures[i] = build.textures.Append( texture );
		}
		// (the lists may have moved)
		w.textures = build.textures.Ptr();
		w.num_textures = build.textures.Num();
	}

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

	if ( !PT_IsTraced( shader ) || shader->Deform() != DFRM_NONE ) {
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
	const double startTicks = Sys_GetClockTicks();

	// here, before any of them is asked for again
	if ( pt.forgetMoving ) {
		pt.forgetMoving = false;
		if ( pt.backend ) {
			for ( i = 0; i < pt.movingRead.Num(); i++ ) {
				const int handles[3] = { pt.movingBase[i], pt.movingDetail[i], pt.movingShine[i] };
				for ( j = 0; j < 3; j++ ) {
					if ( handles[j] >= 0 ) {
						pt.backend->texture_destroy( pt.backend, handles[j] );
					}
				}
			}
		}
		pt.moving.SetNum( 0, false );
		pt.movingMaterials.SetNum( 0, false );
		pt.movingRead.SetNum( 0, false );
		pt.movingBase.SetNum( 0, false );
		pt.movingDetail.SetNum( 0, false );
		pt.movingShine.SetNum( 0, false );
	}

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
	pt.tangents.SetNum( 0, false );
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
		// what only some other view is shown
		if ( def->parms.allowSurfaceInViewID && def->parms.allowSurfaceInViewID != viewID ) {
			continue;
		}
		// The player's own body: the player does not see it, but a mirror
		// shows it, and it stands in the light. Its shadow is cast only if the
		// game casts it (g_showPlayerShadow).
		const bool own = def->parms.suppressSurfaceInViewID && def->parms.suppressSurfaceInViewID == viewID;

		// posed, if it is one that is: the game is asked as it would be were it in view
		const idRenderModel *model = R_EntityDefDynamicModel( def );
		if ( !model ) {
			continue;
		}

		unsigned int flags = 0;
		if ( def->parms.noShadow ) {
			flags |= PT_MAT_NO_SHADOW;
		}
		if ( own ) {
			flags |= PT_MAT_CAMERA_INVISIBLE;
			if ( def->parms.suppressShadowInViewID == viewID ) {
				flags |= PT_MAT_NO_SHADOW;
			}
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
		const bool unturned = axis.IsIdentity();		// as most of what stands about a map is
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
			if ( list && ( origin - viewDef->renderView.vieworg ).Length() < 512.0f ) {
				common->Printf( "      %5i tris %s%s%s\n", tri->numIndexes / 3, shader->GetName(), shader->SurfaceCastsShadow() ? "" : " (no shadow)",
								shader->GetCullType() == CT_TWO_SIDED ? " (two sided)" : "" );
			}
			if ( !tri->tangentsCalculated ) {
				R_DeriveTangents( tri );		// a posed model's normals are worked out when they are first wanted
			}

			// a ray meets a triangle from its front only: one seen from both sides is there twice
			const int sides = shader->GetCullType() == CT_TWO_SIDED ? 2 : 1;
			const int numTris = ( tri->numIndexes / 3 ) * sides;
			int at = pt.triMaterials.Num();
			PT_Grow( pt.triMaterials, at + numTris );
			PT_Grow( pt.positions, ( at + numTris ) * 9 );
			PT_Grow( pt.normals, ( at + numTris ) * 9 );
			PT_Grow( pt.tangents, ( at + numTris ) * 18 );
			PT_Grow( pt.uvs, ( at + numTris ) * 6 );
			for ( k = 0; k < numTris; k++, at++ ) {
				float *p = &pt.positions[at * 9], *n = &pt.normals[at * 9], *uv = &pt.uvs[at * 6], *tn = &pt.tangents[at * 18];

				pt.triMaterials[at] = material;
				// the engine's triangles face the other way round from the tracer's
				const int	of = k / sides;
				const bool	back = ( k % sides ) != 0;
				for ( int c = 0; c < 3; c++ ) {
					const idDrawVert	&v = tri->verts[ tri->indexes[of * 3 + ( back ? c : 2 - c )] ];
					const idVec3		xyz = origin + v.xyz * axis;
					const idVec3		normal = ( back ? -v.normal : v.normal ) * axis;

					p[c * 3] = xyz.x;		p[c * 3 + 1] = xyz.y;		p[c * 3 + 2] = xyz.z;
					n[c * 3] = normal.x;	n[c * 3 + 1] = normal.y;	n[c * 3 + 2] = normal.z;
					// (the same seen from behind: it is the normal that is the other way)
					if ( unturned ) {
						memcpy( &tn[c * 6], &v.tangents[0], 6 * sizeof( float ) );
					} else {
						const idVec3	tu = v.tangents[0] * axis, tv = v.tangents[1] * axis;
						tn[c * 6] = tu.x;		tn[c * 6 + 1] = tu.y;		tn[c * 6 + 2] = tu.z;
						tn[c * 6 + 3] = tv.x;	tn[c * 6 + 4] = tv.y;		tn[c * 6 + 5] = tv.z;
					}
					uv[c * 2] = v.st.x;		uv[c * 2 + 1] = v.st.y;
				}
			}
		}

		span.count = pt.triMaterials.Num() - span.first;
		if ( span.count > 0 ) {
			pt.spans.Append( span );
		}
		if ( list && ( origin - viewDef->renderView.vieworg ).Length() < 512.0f ) {
			common->Printf( "%5i %6i of %6i tris  %s%s%s at %.0f %.0f %.0f\n", i, span.count, numModelTris, def->parms.hModel->Name(), ( flags & PT_MAT_HELD ) ? " (held)" : "",
							( flags & PT_MAT_NO_SHADOW ) ? " (no shadow)" : "", origin.x, origin.y, origin.z );
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
	pt.gatherMs = ( Sys_GetClockTicks() - startTicks ) * 1000.0 / Sys_ClockTicksPerSecond();
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
		ptRead_t			read;
		pt_material_t		material;

		for ( j = 0; j < pt.movingRead.Num(); j++ ) {
			if ( pt.movingRead[j] == shader ) {
				break;
			}
		}
		PT_ReadMaterial( shader, read, j == pt.movingRead.Num() );
		if ( j == pt.movingRead.Num() ) {
			byte	*pixels[3] = { read.base, read.detail, read.shine };
			int		sizes[3][2] = { { read.baseWidth, read.baseHeight }, { read.detailWidth, read.detailHeight }, { read.shineWidth, read.shineHeight } };
			int		at[3] = { -1, -1, -1 };

			for ( int k = 0; k < 3; k++ ) {
				if ( pixels[k] ) {
					pt_texture_t texture;
					texture.width = sizes[k][0];
					texture.height = sizes[k][1];
					texture.pixels = (const uint32_t *) pixels[k];
					at[k] = pt.backend->texture_create( pt.backend, &texture );
				}
			}
			pt.movingRead.Append( shader );
			pt.movingBase.Append( at[0] );
			pt.movingDetail.Append( at[1] );
			pt.movingShine.Append( at[2] );
			PT_FreeRead( read );
		}

		PT_FillMaterial( material, read, pt.movingBase[j], pt.movingDetail[j], pt.movingShine[j], pt.moving[i].flags );
		pt.movingMaterials.Append( material );
	}
}

/*
================
PT_ShowTimes

Where the time of a frame goes, averaged over r_ptShowTimes frames: the whole
frame, then the parts of it that are the tracer's. gather is the models of
what moves, read here; send is the tracer's own work on the processor, the
waits for the card among it; the rest are the card's times for each part.
================
*/
static void PT_ShowTimes( double sendMs ) {
	static double	lastTicks, frameMs, gatherMs, sentMs, stageMs[8];
	static int		frames, stageFrames;
	const char		*names[8];
	pt_stage_t		stages[8];

	const int every = r_ptShowTimes.GetInteger();
	if ( every <= 0 ) {
		frames = 0;
		lastTicks = 0.0;
		return;
	}
	const double now = Sys_GetClockTicks();
	int num = pt.backend->stages ? pt.backend->stages( pt.backend, stages, 8 ) : 0;
	if ( lastTicks == 0.0 || frames == 0 ) {
		frameMs = gatherMs = sentMs = 0.0;
		stageFrames = 0;
		memset( stageMs, 0, sizeof( stageMs ) );
	}
	// a build made with PT_PERF knows each part of the work by name
	static const char	*fineNames[40];
	static double		fineMs[40];
	static int			fineWhere[40], fineNum;
	if ( pt.backend->perf ) {
		pt_stage_t fine[40];
		const int count = pt.backend->perf( pt.backend, fine, 40 );
		for ( int i = 0; i < count && lastTicks != 0.0; i++ ) {
			int at;
			for ( at = 0; at < fineNum; at++ ) {
				if ( fineWhere[at] == fine[i].where && !strcmp( fineNames[at], fine[i].name ) ) {
					break;
				}
			}
			if ( at == fineNum ) {
				if ( fineNum == 40 ) {
					continue;
				}
				fineNames[at] = fine[i].name;
				fineWhere[at] = fine[i].where;
				fineMs[at] = 0.0;
				fineNum++;
			}
			fineMs[at] += fine[i].ms;
		}
	}
	if ( lastTicks != 0.0 ) {
		frameMs += ( now - lastTicks ) * 1000.0 / Sys_ClockTicksPerSecond();
		gatherMs += pt.gatherMs;
		sentMs += sendMs;
		if ( num ) {
			for ( int i = 0; i < num; i++ ) {
				stageMs[i] += stages[i].ms;
			}
			stageFrames++;
		}
		frames++;
	}
	lastTicks = now;
	if ( frames < every ) {
		return;
	}
	idStr line = va( "pt times: frame %.2f ms (%.1f fps), gather %.2f, send %.2f", frameMs / frames, 1000.0 * frames / frameMs, gatherMs / frames, sentMs / frames );
	double card = 0.0;
	for ( int i = 1; i < num && stageFrames; i++ ) {
		line += va( ", %s %.2f", stages[i].name, stageMs[i] / stageFrames );
		card += stageMs[i] / stageFrames;
	}
	common->Printf( "%s, card %.2f; %d triangles moving\n", line.c_str(), card, pt.triMaterials.Num() );
	if ( fineNum ) {
		line = "pt parts:";
		for ( int i = 0; i < fineNum; i++ ) {
			line += va( " %s%s %.2f", fineWhere[i] == 1 ? "card " : "", fineNames[i], fineMs[i] / frames );
		}
		common->Printf( "%s\n", line.c_str() );
	}
	fineNum = 0;
	frames = 0;
}

/*
================
PT_Settings

Each frame, whether or not anything is traced: the settings that stand for
others, and those that take effect only when the scene is handed over again.
They are changed by sliders in a menu as well as typed, so neither is left
to a command.
================
*/
void PT_Settings( void ) {
	static const struct { float scale; int samples, bounces, lightSamples; } presets[] = {
		{ 0.5f, 1, 2, 4 }, { 0.75f, 1, 3, 8 }, { 1.0f, 1, 3, 8 }, { 1.0f, 2, 4, 16 },
	};
	static int		preset = -1;		// what r_ptPreset was left reading
	static int		was;				// which of them the four settings were
	static bool		known;
	static float	last[7];
	static int		changedAt;
	int				i;

	// which of the presets the four settings are now
	int is = 0;
	for ( i = 0; i < 4; i++ ) {
		if ( idMath::Fabs( r_ptScale.GetFloat() - presets[i].scale ) < 0.01f && r_ptSamples.GetInteger() == presets[i].samples
				&& r_ptBounces.GetInteger() == presets[i].bounces && r_ptLightSamples.GetInteger() == presets[i].lightSamples ) {
			is = i + 1;
			break;
		}
	}
	const int asked = r_ptPreset.GetInteger();
	if ( preset >= 0 && asked != preset ) {
		// One was asked for. (Asked to be none of them, it reads so until the
		// four are changed: a choice stepped through in a menu passes that way.)
		if ( asked >= 1 && asked <= 4 ) {
			r_ptScale.SetFloat( presets[asked - 1].scale );
			r_ptSamples.SetInteger( presets[asked - 1].samples );
			r_ptBounces.SetInteger( presets[asked - 1].bounces );
			r_ptLightSamples.SetInteger( presets[asked - 1].lightSamples );
			was = asked;
		}
		preset = asked;
	} else if ( preset < 0 || is != was ) {
		// the four were changed
		preset = was = is;
		if ( asked != is ) {
			r_ptPreset.SetInteger( is );
		}
	}

	// those the scene is made with: once one has stopped changing, it is made again
	const float now[7] = { r_ptGlow.GetFloat(), r_ptSpecular.GetFloat(), r_ptGloss.GetFloat(), r_ptSky.GetFloat(),
		r_ptGlassTint.GetFloat(), (float) r_ptGlass.GetBool(), (float) r_ptMaterials.GetBool() };
	if ( !known ) {
		known = true;
		memcpy( last, now, sizeof( last ) );
	} else if ( memcmp( last, now, sizeof( last ) ) ) {
		memcpy( last, now, sizeof( last ) );
		changedAt = Sys_Milliseconds();
	} else if ( changedAt && Sys_Milliseconds() - changedAt > 300 ) {
		changedAt = 0;
		pt.world = NULL;
		pt.forgetMoving = true;
	}
}

/*
================
PT_Reload_f
================
*/
void PT_Reload_f( const idCmdArgs &args ) {
	pt.world = NULL;
	pt.forgetMoving = true;
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
PT_IsOverlay

Whether a surface of the view is one the tracer leaves out and OpenGL is to
draw over the traced picture.
================
*/
static bool PT_IsOverlay( const viewDef_t *viewDef, const drawSurf_t *surf ) {
	const idMaterial *shader = surf->material;

	if ( !shader ) {
		return false;
	}
	// what is on a computer's screen: the game lays it out flat and stands it in the world
	if ( surf->space != &viewDef->worldSpace && !surf->space->entityDef ) {
		return true;
	}
	// what the game bends as it draws it: flares, sprites
	if ( shader->Deform() != DFRM_NONE ) {
		return true;
	}
	return shader->Coverage() == MC_TRANSLUCENT && !PT_IsGlass( shader );
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
		scene.tangents = pt.tangents.Ptr();
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
	view.antialias = r_ptAntialias.GetBool();
	view.filter = r_ptFilter.GetInteger();
	view.view_mode = r_ptView.GetInteger();
	view.reflections = 2;
	view.refraction = r_ptRefraction.GetBool();
	view.shadow_faces = r_ptShadowFaces.GetBool();
	view.reflection_rate = 1.0f;
	view.light_samples = r_ptLightSamples.GetInteger();
	view.firefly_clamp = r_ptFireflyClamp.GetFloat();
	view.texture_filter = 1;
	view.denoise = r_ptDenoise.GetInteger();
	view.history = r_ptHistory.GetInteger();
	view.reflection_history = 1;
	view.saturation = r_ptSaturation.GetFloat();
	view.contrast = r_ptContrast.GetFloat();
	view.tonemap = r_ptTonemap.GetInteger();
	if ( PT_VIEW_IS_MEASURE( view.view_mode ) ) {
		view.exposure = 1.0f;
		view.auto_exposure = 0;
		view.tonemap = 2;
	}
	if ( r_ptView.IsModified() ) {
		r_ptView.ClearModified();
		view.restart = 1;
	}

	const double sendTicks = Sys_GetClockTicks();
	pt.backend->render_view( pt.backend, &view );
	// (the overlay is empty and stays so: none of it has changed since it was first sent)
	static const pt_rect_t noRect = { 0, 0, 0, 0 };
	pt.backend->present( pt.backend, pt.overlay.Ptr(), &noRect, pt.overlaySent ? 0 : -1 );
	pt.overlaySent = true;
	PT_ShowTimes( ( Sys_GetClockTicks() - sendTicks ) * 1000.0 / Sys_ClockTicksPerSecond() );

	// The depth of the scene as OpenGL sees it, which is all it draws of it:
	// what it lays over the traced picture is hidden where the scene hides it.
	drawSurf_t	**drawSurfs = (drawSurf_t **) &viewDef->drawSurfs[0];
	const int	numDrawSurfs = viewDef->numDrawSurfs;
	const bool	overlay = r_ptOverlay.GetBool() && numDrawSurfs > 0;
	if ( overlay ) {
		backEnd.depthFunc = GLS_DEPTHFUNC_EQUAL;
		RB_BeginDrawingView();
		RB_DetermineLightScale();
		RB_STD_FillDepthBuffer( drawSurfs, numDrawSurfs );
	}

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

	// what the tracer leaves out, drawn as the game draws it
	if ( overlay ) {
		drawSurf_t	**mine = (drawSurf_t **) _alloca( numDrawSurfs * sizeof( drawSurf_t * ) );
		int			count = 0;

		for ( int i = 0; i < numDrawSurfs; i++ ) {
			if ( PT_IsOverlay( viewDef, drawSurfs[i] ) ) {
				mine[count++] = drawSurfs[i];
			}
		}
		if ( count ) {
			qglViewport( tr.viewportOffset[0] + viewDef->viewport.x1, tr.viewportOffset[1] + viewDef->viewport.y1,
				viewDef->viewport.x2 + 1 - viewDef->viewport.x1, viewDef->viewport.y2 + 1 - viewDef->viewport.y1 );
			qglScissor( tr.viewportOffset[0] + viewDef->viewport.x1 + viewDef->scissor.x1, tr.viewportOffset[1] + viewDef->viewport.y1 + viewDef->scissor.y1,
				viewDef->scissor.x2 + 1 - viewDef->scissor.x1, viewDef->scissor.y2 + 1 - viewDef->scissor.y1 );
			backEnd.currentScissor = viewDef->scissor;
			GL_State( GLS_DEFAULT );
			GL_Cull( CT_FRONT_SIDED );

			// (those that work on the picture so far come last, and are drawn once it has been copied for them)
			const int processed = RB_STD_DrawShaderPasses( mine, count );
			if ( processed < count ) {
				RB_STD_DrawShaderPasses( mine + processed, count - processed );
			}
		}
	}

	return true;
}
