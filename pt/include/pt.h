/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Jonathan Ferguson */
#ifndef PT_H
#define PT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*pt_log_fn)(const char *msg);

typedef struct pt_create_s
{
	void		*hinstance;		/* Windows: HINSTANCE of the host. X11: the Display * */
	void		*hwnd;			/* Windows: HWND to present into. X11: the Window, cast */
	int			width, height;	/* client area in pixels */
	pt_log_fn	log;			/* may be NULL */
	int			simd;			/* CPU backend: 0 = built for the widest instructions the
								   processor has, 1 = for SSE only, to compare the two */
	int			shared;			/* RTX on Windows: nothing is put on the window, and
								   hinstance and hwnd are not looked at. present draws
								   into a picture that another graphics API in the
								   same process can open, see shared_image, and
								   returns when it is finished */
} pt_create_t;

/* The picture a backend made with shared draws into: width * height pixels of
   four bytes R,G,B,A, top row first. handle is a Windows handle to its memory,
   of the opaque kind Vulkan exports (OpenGL: GL_HANDLE_TYPE_OPAQUE_WIN32_EXT),
   size bytes long with the picture at its start. It belongs to the backend. */
typedef struct pt_shared_image_s
{
	void		*handle;
	uint64_t	size;
	int			width, height;
} pt_shared_image_t;

/*
World description. Right handed, any consistent length unit. Colours in
textures are display referred (sRGB-like); emission, light intensity and sky
scale are linear radiometric values in one consistent unit.

Triangles are one sided for emission: they emit from the side where the
vertices appear counter clockwise.
*/

#define PT_MAT_SKY				1u	/* shows the sky; nothing else applies */
#define PT_MAT_ALPHA_TEST		2u	/* texels with alpha under half are holes */
#define PT_MAT_EMIT_TEXTURE		4u	/* emitted radiance is emission * texel */
#define PT_MAT_CAMERA_INVISIBLE	8u	/* not seen directly, still lights and shadows */
#define PT_MAT_BLACK			16u	/* reflects nothing; it can still emit */
#define PT_MAT_WAVES			32u	/* a liquid surface: its normal ripples over time */
#define PT_MAT_WARP				128u	/* the texture swims, as old engines drew liquids */
#define PT_MAT_EMIT_BRIGHT		64u	/* only the texture's bright texels emit: screens,
									   buttons, indicator lights */
#define PT_MAT_HELD				256u	/* carried by the eye, as a weapon in hand is: only
									   the eye sees it, not a mirror nor light finding
									   its way about, and it casts shadows on itself
									   alone; with motion blur it is seen from where the
									   eye is when the shutter closes, however the eye
									   moved. RTX backend only */
#define PT_MAT_METAL_TEXTURE	512u	/* the blue of normal_texture is how metallic the
									   surface is there, in place of metallic; the
									   normal's z is worked out from its x and y */
#define PT_MAT_METAL_PAINTED	1024u	/* the texture was painted to be looked at, and
									   shows metal as dark as it looks in a dim room:
									   where the surface is metal, what it reflects
									   is worked out from the colour, see pt_view_t's
									   metal_colour. What it emits is not changed. */
#define PT_MAT_SAMPLED			2048u	/* scene only: what this emits is also given as a
									   light in lights, so a path that comes upon it
									   counts its glow only where lights are not looked
									   for: from the eye and in a sharp reflection */
#define PT_MAT_NO_SHADOW			8192u	/* casts no shadow: light on its way from a light
									   passes it as if it were not there. It is seen
									   and lit as anything else is. For a lamp's own
									   housing, where the lamp is given as a point
									   inside it */
#define PT_MAT_REFRACT			16384u	/* a pane of glass (alpha under 1, smooth) that
									   bends what is seen through it, with pt_view_t's
									   refraction: the view is bent going in, by the
									   face as its normal map tilts it, and back again
									   coming out of the pane's far face, which is
									   looked for behind it and failing that taken to
									   be 2 units in. What is behind shows shifted,
									   more the thicker the pane and the more it is
									   seen from the side, and wavers where the glass
									   is uneven. RTX backend only */
#define PT_MAT_MIRROR			32768u	/* a flat mirror (roughness 0, metallic 1): the eye
									   looks through it at what it shows, which is then
									   as sharp, and followed from frame to frame as
									   well, as what is seen directly. Without the flag
									   such a surface still mirrors. RTX backend only */
#define PT_MAT_FILTER			65536u	/* see-through (alpha under 1): the texture is the
									   colour of the glass, which what is seen through
									   it is multiplied by, texel by texel. It covers
									   nothing and is not lit. RTX backend only */
#define PT_MAT_DECAL				131072u	/* a picture laid on a surface (alpha 0): the eye
									   looks through it, and its colour is added to
									   that of the solid surface it then meets, which
									   is lit as one with it. Nothing else meets it:
									   it casts no shadow and no light bounces off
									   it. Black in it adds nothing. RTX backend only */
#define PT_MAT_EMIT_MAPPED		4096u	/* emission_texture says where on the surface the
									   material's light comes from, and stands for
									   texture in all that is said of emission: a
									   point gives off emission times its texel there
									   over its average texel, emission_seen applies,
									   and a world's material is sampled as a light
									   as it would be without the map. For a lamp in
									   its frame: the lamp gives off all the light
									   and the frame none */

typedef struct pt_texture_s
{
	int				width, height;
	const uint32_t	*pixels;	/* R,G,B,A bytes, top row first */
} pt_texture_t;

typedef struct pt_material_s
{
	int			texture;		/* -1 for plain white. In a world, an index into
								   its textures; in a scene, a handle from
								   texture_create */
	float		emission[3];	/* average emitted radiance. With a texture the
								   radiance at a point is this times
								   texel / average texel, unless
								   PT_MAT_EMIT_TEXTURE */
	float		emission_seen;	/* if above 0: the radiance shown to the eye for a
								   white texel, in place of the real emission, so
								   a bright lamp keeps its look */
	float		alpha;			/* 1 = opaque; less lets light through */
	float		roughness;		/* 0 = mirror, 1 = fully rough */
	float		metallic;		/* 0 = dielectric, 1 = metal */
	int			emission_texture;	/* 0 for none, else 1 + a texture, numbered as
								   texture is, that says what glows and in what
								   colour: emitted light is it times emission.
								   Found by paths only, not sampled as a light.
								   emission_seen applies: to the eye it is the
								   map times that. See also PT_MAT_EMIT_MAPPED */
	int			normal_texture;	/* -1 for none; same numbering as texture. RGB is a
								   tangent space normal (x along u, y along v),
								   alpha replaces roughness. See also
								   PT_MAT_METAL_TEXTURE */
	int			specular_texture;	/* 0 for none, else 1 + a texture, numbered as
								   texture is: the colour the surface reflects,
								   seen head on, in place of what metallic and
								   the texture would make it. The texture is then
								   its matte colour alone, less only what would
								   make the two together reflect more light than
								   falls on them. For materials painted for an
								   engine that lights a surface as a matte colour
								   plus a highlight of a colour of its own. RTX
								   backend only */
	int			anim_next;		/* world only: the material shown one animation
								   step later, or -1 */
	int			wave_map;		/* liquids: texture_create handle + 1 of a wave
								   picture (R, G slopes in x and y about 0.5; B, A
								   the height above the triangle in sixteen bits,
								   B the upper, 16 units from 0 to 65535 and level
								   half way), 0 = none; replaces the PT_MAT_WAVES
								   ripples. Level triangles with one are drawn
								   where the waves stand, see wave_reach */
	int			caustic_map;	/* handle + 1 of a picture of how sharply the
								   waves curve, which gathers and spreads the
								   light going through, and in A of how high the
								   liquid has lately stood: see pt_water_caustics
								   in pt/water. 0 = none. A is 0 where the picture
								   covers no liquid, and the waves' surface is
								   not looked for there */
	int			foam_map;		/* handle + 1 of the froth on the liquid; 0 =
								   none. R is how much of the surface it covers,
								   G how old it is (0 fresh, 255 old: older froth
								   lies in thinner strings), B and A how fast the
								   liquid carries it in x and y, units per second
								   + 128. It is drawn as a pale layer of bubbles
								   over the liquid, which shows through where it
								   is thin */
	float		wave_rect[4];	/* the maps cover world x, y from [0], [1] and are
								   1 / [2], 1 / [3] across */
	float		absorb[3];		/* liquids: share of light lost per unit of
								   distance through it */
	float		scroll[2];		/* texture repeats per second it slides by, in u and v */
	uint32_t	flags;
} pt_material_t;

typedef struct pt_point_light_s
{
	float		origin[3];
	float		intensity[3];	/* radiant intensity; irradiance is this * cos / d^2 */
	int			style;			/* world only: index into the view's light_styles */
	float		direction[3];	/* a spotlight shines along this (unit) ... */
	float		cone_cos;		/* ... within the cone with this cosine of its half
								   angle; 0 = shines all round */
	float		reach;			/* points, not balls: above 0 the light goes no further than
								   this. It thins with the square of the distance d
								   and is also times (1 - (d / reach)^4)^2, which is
								   1 near the light and falls to nothing at reach.
								   For a light an engine draws as a lit volume of a
								   set size, which would otherwise be seen across
								   the map. 0 = no end. One of the map's with a
								   reach is looked for only from the parts of the
								   map it reaches, and there the most it is weighed
								   among is the 24 that put most light on that
								   part: what is hidden from all of a part's
								   surfaces counts for little */
	float		core;			/* points, not balls: above 0, nothing nearer the
								   light than this is lit more brightly than what
								   is this far from it: the distance d is taken as
								   no less than core. For a light an engine draws
								   no brighter close to than its colour, which as
								   a true point would burn out all that is near it */
	float		box_origin[3];	/* points, not balls. With all of box_size above 0 */
	float		box_size[3];	/* the light is an engine's lit volume: it lights
								   only what is inside the box that is box_size to
								   each side of box_origin, along the world's axes,
								   brightest at box_origin and fading to nothing
								   at the box's faces: times 1 - (d / size)^2 for
								   each axis. This is in place of reach's fade;
								   reach is still to be given, as far as any of
								   the box is from origin, which need not be
								   inside it: origin is where the light comes
								   from, the box what it lights. */
	int			no_shadow;		/* points, not balls: nothing stops its light, as
								   an engine's fill light that casts no shadows */
	float		radius;			/* scene only: above 0 the light is a ball of this
								   size, as bright all over, which gives off the
								   same light in all as the point would: its
								   radiance is intensity / (pi * radius^2). Its
								   shadows are soft. Nothing is drawn of it: for
								   that, triangles that emit the same
								   (PT_MAT_SAMPLED), no larger than it is. */
} pt_point_light_t;

typedef struct pt_world_s
{
	const pt_texture_t	*textures;
	int					num_textures;
	const pt_material_t	*materials;
	int					num_materials;

	const float			*positions;			/* 3 per vertex */
	const float			*uvs;				/* 2 per vertex, 1.0 = one texture repeat */
	const float			*normals;			/* 3 per vertex, or NULL for flat shading */
	const float			*tangents;			/* 6 per vertex, or NULL: the way u runs across
											   the surface there and the way v does, as the
											   normal maps were made for. Without them the
											   two are worked out for each triangle from its
											   corners, which shows the triangles wherever a
											   normal map was made for a smooth surface.
											   RTX backend only, as normals are here */
	int					num_vertices;
	const uint32_t		*indices;			/* 3 per triangle */
	const uint32_t		*tri_materials;		/* 1 per triangle */
	int					num_triangles;

	const pt_point_light_t	*lights;
	int						num_lights;

	/*
	Sky cube, or -1s for a black sky. Face order +X -X +Y -Y +Z -Z. For the
	face on axis a, with b and c the next two axes in cyclic order (X: Y,Z
	Y: Z,X  Z: X,Y), a direction d maps to
		u = (d[b] / |d[a]| + 1) / 2,  v = (d[c] / |d[a]| + 1) / 2
	with v = 0 at the texture's top row.
	*/
	int					sky_textures[6];
	float				sky_scale;			/* radiance of a white sky texel */
} pt_world_t;

/*
What moves: rebuilt by the host every frame, in world space. Its emitting
triangles light the scene but are found by chance, so anything that should
light well also belongs in lights (see PT_MAT_SAMPLED).
*/
typedef struct pt_scene_s
{
	const pt_material_t	*materials;
	int					num_materials;

	const float			*positions;			/* 9 per triangle */
	const float			*uvs;				/* 6 per triangle */
	const float			*normals;			/* 9 per triangle, or NULL for flat shading */
	const float			*tangents;			/* 18 per triangle, or NULL: for each corner the
											   way u runs and the way v does; see pt_world_t */
	const float			*prev_positions;		/* 9 per triangle: where it was last frame; NULL
											   if nothing is known to have moved */
	const uint32_t		*tri_materials;		/* 1 per triangle */
	int					num_triangles;

	const pt_point_light_t	*lights;
	int						num_lights;
} pt_scene_t;

/*
Ways of drawing the scene other than as it is, for checking a renderer and
for pictures.

Clay and mirror override the materials. Lights are as they are, and what
glows keeps its glow. The sky, glass, and what only emits (PT_MAT_BLACK) are
left alone.
*/
#define PT_VIEW_NORMAL	0
#define PT_VIEW_CLAY	1	/* every surface matte mid grey, fully rough, not metal,
						   whatever its textures say. Liquids too: to the eye
						   they are solid, though light still passes them. */
#define PT_VIEW_MIRROR	2	/* every surface as smooth as the backend can make
						   one; its colour and whether it is metal are kept */
/*
The white furnace: a test of whether paths keep the light they carry. Every
surface, glass and liquids too, is matte and reflects all the light that
falls on it; no light or glow is lit, what only emits is not there, and a
path that reaches the sky, leaves the map or runs out of bounces brings back
PT_FURNACE_LIGHT. A tracer that neither makes nor loses light draws every
pixel at that level; brighter is light made, darker is light lost.
*/
#define PT_VIEW_FURNACE	3
#define PT_FURNACE_LIGHT	0.5f
#define PT_VIEW_LIGHTING	4	/* the light alone: what the eye sees is white, of
							   the material it is, lit by the scene as it is */
/* Light that reaches what the eye sees straight from a light, the sky or the
   air's own glow, and what glows seen directly; and all the rest, which has
   bounced or been mirrored on the way. The two add up to the picture. */
#define PT_VIEW_DIRECT		5
#define PT_VIEW_INDIRECT	6
/* One thing known of the first surface the eye meets, glass included, unlit;
   the sky is black. But for the base colour, the number is what is shown:
   after the display's own curve a value of 0.5 is a pixel of 128. */
#define PT_VIEW_BASE_COLOUR	7
#define PT_VIEW_NORMALS		8	/* the shading normal, in the world, 0.5 + 0.5 n */
#define PT_VIEW_ROUGHNESS	9
#define PT_VIEW_METAL		10
#define PT_VIEW_GLOW		11	/* what it emits, held to 1 */
/* How many times the paths from each pixel bounced, on average, after the
   first surface: black none, blue 1, green 2 (cyan between), yellow 3, red 4
   or more, whatever the settings. Under 1, a blue darker than blue, is a
   pixel some of whose paths did not bounce at all: what reflects nothing,
   such as a light, mixed with what does. */
#define PT_VIEW_BOUNCES		12
/* What the pixel cost: every ray traced for it this frame, the eye's, the
   bounces' and those sent to lights to see whether they are in shadow, all
   its samples together. Each colour is twice the one before: black none,
   blue PT_COST_BLUE, green twice that (cyan between), yellow four times,
   red eight times or more, whatever the settings. */
#define PT_VIEW_COST		13
#define PT_COST_BLUE		4.0f
#define PT_NUM_VIEWS		14
/* The furnace, the single values and the bounce count are numbers to be
   read off the picture: send exposure 1, no auto exposure, the clipped tone
   curve, no bloom and no fog with them. */
#define PT_VIEW_IS_MEASURE(mode)	((mode) == PT_VIEW_FURNACE || (mode) >= PT_VIEW_BASE_COLOUR)

/* one 3D view */
typedef struct pt_view_s
{
	/* the part of the window it covers, in pixels from the top left */
	int		x, y, width, height;
	float	time;

	float	origin[3];
	float	forward[3], right[3], up[3];	/* orthonormal */
	float	fov_x, fov_y;					/* degrees */

	/* Motion blur. With blur set, each path starts from where the eye was at
	   a moment of its own, chosen at random between the shutter opening,
	   when the eye was at the open_ pose, and its closing, when it is at the
	   pose above. Nothing of an earlier frame is looked up: send restart. */
	int		blur;
	float	open_origin[3];
	float	open_forward[3], open_right[3], open_up[3];

	const pt_scene_t	*scene;				/* may be NULL */

	const float	*light_styles;			/* brightness of each world light style, 1 = normal */
	int			num_light_styles;
	const float	*light_scales;			/* 3 for each of the lights load_world was given, in
										   that order: the light's intensity is times this,
										   red, green and blue, this frame. For lights that
										   flicker, change colour or are switched, where
										   there are more of them than light styles. A light
										   is looked for as often as its intensity at
										   load_world earned it, so one that may come on
										   should not be given as dark. NULL = all 1 */
	int			num_light_scales;		/* lights, not floats */
	int			anim_frame;				/* which step animated materials are on */
	float		medium_absorb[3];		/* the eye is inside a liquid that absorbs this much */
	float		sky_axis[3];			/* the sky turns about this (unit) axis ... */
	float		sky_angle;				/* ... by this many degrees */

	int		restart;		/* forget what earlier frames gathered: this one is
						   made from nothing, as the first ever is. Exposure
						   still follows on from before. */

	/* quality settings; a backend may ignore what it has no use for */
	float	scale;			/* internal resolution as a fraction of the view */
	int		samples;		/* paths per pixel per frame */
	int		bounces;		/* maximum path length after the first hit */
	float	exposure;
	int		antialias;	/* blend frames over time to smooth edges */
	int		filter;		/* what is done about noise. 2: frames are blended over time and the
						   picture filtered. 1: neither; a frame stands alone, except
						   that while the eye is at rest frames add up. 0: neither, ever */
	int		debug;			/* 0 = the picture; otherwise one part of it, see pt_debug */
	int		view_mode;		/* PT_VIEW_: the scene drawn some other way than as it is.
						   Send restart when it changes, or the old picture
						   bleeds into the new one. */

	/* reflections */
	int		reflections;		/* 0 none, 1 glass and liquids, 2 every shiny surface */
	int		reflection_bounces;	/* how far a reflected path is followed; 1 shows
								   reflected things under direct light only */
	float	reflection_rate;	/* scales how often a rough surface gets a
								   reflection path; 1 = the backend's own choice */
	int		refraction;			/* liquids bend the view */
	int		shadow_faces;		/* which side of a surface stops light on its way
								   from a light. 0: the side turned away from the
								   light, as it is met by a ray sent to the light.
								   1: the side turned to the light, as an engine
								   that draws shadow volumes casts them. Closed
								   shapes shadow the same either way. With 1 a
								   light set inside a solid shines out of it, as
								   in such an engine, whose maps count on it */
	float	wave_strength;		/* ripples on liquids; 0 = flat, 1 = normal */
	float	wave_reach;			/* no wave picture holds a height further than this
								   from level this frame, in units */
	float	water_shafts;		/* how much liquids scatter the light in them
								   towards the eye, which shows as shafts; 0 =
								   not at all, 1 = normal */
	float	water_wet;			/* how wet the banks of simulated liquids show
								   where they have stood; 0 = not at all, 1 = normal */
	float	metal_colour;		/* PT_MAT_METAL_PAINTED: how much of the light a
								   metal painted mid dark (1.2% of white, as old
								   game art paints steel) reflects. One painted
								   four times as bright reflects twice that, none
								   less than it was painted, and the hue is kept.
								   0 = the colour as it is. */

	/* lighting */
	int		light_samples;		/* lights weighed per point the eye sees; half
								   as many further along a path */
	float	firefly_clamp;		/* brightest a single path may be */

	/* image */
	int		texture_filter;		/* smooth textures where seen directly */
	int		denoise;			/* passes of the spatial filter, 0-4 */
	int		history;			/* frames of lighting kept while things change */
	int		reflection_history;	/* what mirrors show is followed where it appears to
								   be, not where the surface is; 0 = where the surface is */
	int		threads;			/* 0 = all */
	int		auto_exposure;		/* adapt exposure to the scene; exposure then
								   scales the result */
	int		tonemap;			/* 0 filmic, 1 neutral, 2 clipped */
	float	saturation;			/* 1 = unchanged */
	float	contrast;			/* 1 = unchanged */
	int		fog;				/* light scattering in the air: haze and light shafts */
	float	fog_density;		/* share of light scattered per unit of distance */
	int		fog_samples;		/* points along each view ray at which the air's
								   light is looked for, a frame; below 1 = 1 */
	int		fog_history;		/* frames of the air's light kept while things
								   change, in place of history: the air has no
								   surface to be followed by, so its light
								   trails what moves; fewer frames trail less
								   and are noisier. Below 1 = as history */
	float	react;				/* how readily what was gathered is let go where
								   the light is found to have changed, so that it
								   does not trail a light that moves, flashes or
								   goes out: 0 = never, 1 = at once. Such places
								   are noisier for a few frames. RTX only */
	float	bloom;				/* glow around what is brighter than white; 0 = none */
	float	bloom_max;			/* the most over white that anything adds to the
								   glow, in whites: up to half of this it adds
								   all it has, then less and less. Keeps a lamp
								   hundreds of times white from drowning the
								   picture. 0 = no limit */

	/* also make the planes that are only for export, see read_plane: what
	   every light would put on each pixel with nothing in the way, and the
	   surface's emission, metalness and identity. Costs a loop over every
	   light per pixel: not for play */
	int		planes;
} pt_view_t;

/*
Per-pixel planes of the last view rendered, at the size it was traced, for
making training data: see neural/README.md. Each is read with read_plane as
floats, top row first, the channels of a pixel together. Light is linear,
before exposure; where the view ends at the sky or nothing, depth is -1 and
the surface planes are 0.
*/
#define PT_PLANE_POSITION			0	/* 3: where in the world the pixel's surface is; the sky: far along the ray */
#define PT_PLANE_NORMAL				1	/* 3: shading normal, in the world */
#define PT_PLANE_DEPTH				2	/* 1: distance along the eye ray to the first solid surface, -1 none */
#define PT_PLANE_ALBEDO				3	/* 3: diffuse reflectance as the light is multiplied by it: with
									   what the air and the liquid on the way let through */
#define PT_PLANE_SPECULAR			4	/* 3: specular reflectance, the same way */
#define PT_PLANE_ROUGHNESS			5	/* 1 */
#define PT_PLANE_EMISSION			6	/* 3: radiance the surface gives off towards the eye */
#define PT_PLANE_METALLIC			7	/* 1 */
#define PT_PLANE_MATERIAL			8	/* 1: material index, -1 none */
#define PT_PLANE_TRIANGLE			9	/* 2: triangle index, and 1 if it is of the frame (what moves) rather than the map */
#define PT_PLANE_DIRECT_DIFFUSE		10	/* 3: what every light, the map's and the frame's, would put on the
									   surface with nothing in the way: irradiance, to be times
									   albedo / pi. Only with the view's planes set */
#define PT_PLANE_DIRECT_SPECULAR	11	/* 3: the same through the specular lobe, as radiance. Only with planes */
#define PT_PLANE_RAY_DIFFUSE		12	/* 3: the direct light this frame's paths found, with shadows, over
									   albedo / pi: with one path and no bounces, one shadow ray's worth */
#define PT_PLANE_RAY_SPECULAR		13	/* 3: the same through the specular lobe, over the specular reflectance */
#define PT_PLANE_LIGHT_DIFFUSE		14	/* 3: the diffuse light gathered over the frames since restart, over albedo / pi */
#define PT_PLANE_LIGHT_SPECULAR		15	/* 3: the specular light gathered, over the specular reflectance */
#define PT_PLANE_LIGHT_LAYERS		16	/* 3: light of what is in front of the surface: glass, water, the air's glow */
#define PT_PLANE_LIGHT_EXTRA		17	/* 3: exact light that is not gathered: emission and the frame's point lights */
#define PT_PLANE_PICTURE			18	/* 3: the picture as put together, linear, at the view's exposure */
#define PT_NUM_PLANES				19

/* one part of the work on a view, and how long it took */
typedef struct pt_stage_s
{
	const char	*name;		/* a short word, the same from frame to frame */
	float		ms;
	int			where;		/* only from perf: 0 the processor at work, 1 the
							   card, 2 the processor waiting for the card or
							   the screen */
} pt_stage_t;

/* a rectangle of pixels */
typedef struct pt_rect_s
{
	int		x, y, width, height;
} pt_rect_t;

/*
A backend owns everything between "here is the scene" and pixels on screen.

One frame is: an optional render_view, then exactly one present. Anything
outside the view, and the whole window when render_view was not called, is
black under the overlay.

The overlay is width*height pixels, bytes R,G,B,A in memory, premultiplied
alpha, top row first. It is composited over the view.
*/
typedef struct pt_backend_s pt_backend_t;
struct pt_backend_s
{
	const char	*name;
	const char	*device;	/* what does the tracing: the processor, or the card */
	void	(*destroy)(pt_backend_t *self);

	/* copies everything it needs; NULL unloads the world */
	void	(*load_world)(pt_backend_t *self, const pt_world_t *world);

	/* textures for scene materials. Returns a handle, or -1 */
	int		(*texture_create)(pt_backend_t *self, const pt_texture_t *texture);
	void	(*texture_destroy)(pt_backend_t *self, int handle);
	/* new pixels for a texture, same size */
	void	(*texture_update)(pt_backend_t *self, int handle, const uint32_t *pixels);

	void	(*render_view)(pt_backend_t *self, const pt_view_t *view);
	/* Shows the last view rendered with the overlay over it: width x height
	   premultiplied R,G,B,A pixels. changed lists the parts of the overlay
	   that may differ from the one given last time, so that a backend that
	   keeps a copy has only those to fetch; num_changed < 0: any of it may. */
	void	(*present)(pt_backend_t *self, const uint32_t *overlay, const pt_rect_t *changed, int num_changed);

	/* NULL in a backend that cannot share its picture. Returns 0 if this one
	   was not made with shared. */
	int		(*shared_image)(pt_backend_t *self, pt_shared_image_t *out);

	/* about the last view rendered, valid until the next call: a few short
	   lines of text separated by '|' */
	const char *(*stats)(pt_backend_t *self);

	/* Where the time went on the latest view whose times are known; a card
	   only knows them a frame late. Fills in up to max stages, in the order
	   they ran, and returns how many: 0 if there is nothing new since it was
	   last asked. */
	int		(*stages)(pt_backend_t *self, pt_stage_t *stages, int max);

	/* Only in a build made with PT_PERF, NULL in any other. A finer look at
	   the same: every part of the frame that is timed, what was done here
	   since perf was last asked first, then the card's parts of the latest
	   view whose times are known. Asked once a frame, after present. Fills
	   in up to max and returns how many. */
	int		(*perf)(pt_backend_t *self, pt_stage_t *stages, int max);

	/* the picture last presented: width*height pixels, bytes R,G,B,A, top row
	   first, with or without the overlay. Returns 0 if it cannot. */
	int		(*read_pixels)(pt_backend_t *self, uint32_t *pixels, int with_overlay);

	/* The planes of the last view rendered, PT_PLANE_: plane_size says how
	   large they are, read_plane fills width*height*channels floats and
	   returns how many channels, 0 if it cannot. NULL in a backend that has
	   no planes to give. */
	int		(*plane_size)(pt_backend_t *self, int *width, int *height);
	int		(*read_plane)(pt_backend_t *self, int plane, float *out);
};

/* both return NULL on failure with a reason in err */
pt_backend_t *pt_cpu_create(const pt_create_t *ci, char *err, int errlen);
pt_backend_t *pt_rtx_create(const pt_create_t *ci, char *err, int errlen);

#ifdef __cplusplus
}
#endif

#endif
