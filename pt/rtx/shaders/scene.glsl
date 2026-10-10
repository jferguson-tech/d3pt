// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Jonathan Ferguson
//
// What every pass shares: how the scene is laid out on the card, and, for
// the passes that trace, surfaces, lights and paths. It follows the CPU
// backend (pt/cpu/pt_trace.cpp) function for function, so that the two make
// the same picture.

// same values as PT_MAT_* in pt.h
const uint MAT_SKY = 1u;
const uint MAT_ALPHA_TEST = 2u;
const uint MAT_EMIT_TEXTURE = 4u;
const uint MAT_CAMERA_INVISIBLE = 8u;
const uint MAT_BLACK = 16u;
const uint MAT_WAVES = 32u;
const uint MAT_EMIT_BRIGHT = 64u;
const uint MAT_EMIT_MAPPED = 4096u;
const uint MAT_WARP = 128u;
const uint MAT_METAL_TEXTURE = 512u;
const uint MAT_METAL_PAINTED = 1024u;

// pt_view_t's view_mode, as in pt.h
const int VIEW_NORMAL = 0;
const int VIEW_CLAY = 1;
const int VIEW_MIRROR = 2;
const int VIEW_FURNACE = 3;
const int VIEW_LIGHTING = 4;
const int VIEW_DIRECT = 5;
const int VIEW_INDIRECT = 6;
const int VIEW_BASE_COLOUR = 7;
const int VIEW_NORMALS = 8;
const int VIEW_ROUGHNESS = 9;
const int VIEW_METAL = 10;
const int VIEW_GLOW = 11;
const int VIEW_BOUNCES = 12;
const int VIEW_COST = 13;
const float COST_BLUE = 4.0;
const float FURNACE_LIGHT = 0.5;

// Material.bits
const uint BIT_EMISSIVE = 1u;
const uint BIT_SAMPLED = 2u;		// reached through the light lists, so not counted when hit by chance
const uint BIT_SWELL = 4u;		// a simulated liquid, met where its waves stand and not at its triangles: see WaveCross

const float PI = 3.14159265;
const float INV_PI = 0.31830989;
const float RAY_OFFSET = 0.03;				// keeps rays off the surface they leave
const float LIGHT_SAMPLED_ROUGHNESS = 0.25;	// below this a highlight is found by its own path
const float MIN_ALPHA = 0.002;				// GGX gets numerically touchy below this
const float GLOBAL_LIGHT_CHANCE = 0.2;		// how often a light is picked map wide instead of nearby
const int PER_CELL = 24;					// lights listed per cell of the grid
const float MIN_DEMODULATE = 0.02;
const float WATER_INDEX = 1.33;
const float CAUSTIC_MAX = 4.0;				// the most that waves may brighten light by
const float RIPPLE_SLOPE = 0.1;			// of each train of fine ripples on a liquid, at their most			// reflectance floor when lighting is divided by it

struct Tri
{
	vec2	uv0, uv1, uv2;
	uint	material;
	uint	pad;
};

struct Material
{
	int		texture;			// slots in textures, or -1
	uint	flags;
	float	alpha;
	float	roughness;
	vec4	emission;			// rgb: average emitted; a: radiance when seen directly, if set
	vec4	emission_per_texel;	// rgb: times the texel gives what is emitted; a: metallic
	int		normal_texture;
	int		emission_map;
	int		anim_next;			// the material shown next in its animation, or -1
	int		anim_length;
	vec4	absorb;				// rgb: how fast a liquid soaks up light
	vec4	wave_rect;			// where a simulated liquid's maps lie
	vec2	scroll;
	int		wave_map;			// slots, or -1
	int		caustic_map;
	uint	bits;
	int		foam_map;
	uint	pad0, pad1;
};

struct Light
{
	vec3	origin;		// point lights; a triangle's first corner
	uint	tri;		// the triangle, or for a point light POINT_LIGHT plus its light style
	vec3	emission;	// radiance for triangles, intensity for points
	float	pdf;		// chance of being picked map wide
	vec3	dir;		// spotlights: where it points; triangles: from the first corner to the second
	float	cone_cos;	// and how wide; 0 = all round
	vec3	edge;		// triangles: from the first corner to the third
	float	radius;		// the frame's: above 0 a ball of light, not a point
};
const uint POINT_LIGHT = 0xffffff00u;

layout(set = 0, binding = 0) uniform accelerationStructureEXT scene;

layout(std140, set = 0, binding = 1) uniform Frame
{
	vec4	origin;			// w: tangent of half the horizontal field of view
	vec4	forward;		// w: tangent of half the vertical one
	vec4	right;			// w: this frame's offset within the pixel, x
	vec4	up;				// w: and y
	vec4	prev_origin;	// the same for the frame before
	vec4	prev_forward;
	vec4	prev_right;
	vec4	prev_up;
	ivec4	sky_a;			// textures of the sky's faces +X -X +Y -Y,
	ivec4	sky_b;			// +Z -Z; -1 where there is none
	vec4	sky_turn;		// xyz: the axis the sky turns about; w: sine of the angle
	vec4	sky_misc;		// cosine, brightness of a white texel, integral of its luminance, time
	ivec4	counts;			// lights of the map, lights of the frame, frame number, animation step
	ivec4	bases;			// y, w: first triangle of the map's glass and of the frame's; x: the view mode; z: what is done about noise, see pt_view_t
	ivec4	grid_dims;		// xyz; w: there is a grid
	vec4	grid_origin;	// xyz; w: one over the cell size
	ivec4	table_at;		// in tables: how often each of the map's lights is kept when its place is drawn (see DirectLights); for each cell's list, each light's chance of being picked, then how often each is kept; sky chance
	ivec4	table_at2;		// in tables: sky cdf; sky resolution; in indices: grid lights, grid counts
	ivec4	settings;		// bounces, light samples, reflections, reflection bounces
	vec4	settings_f;		// brightest a path may be, reflection rate, wave strength, fog density
	ivec4	settings2;		// refraction, smooth textures, paths a pixel, debug view
	vec4	medium;			// xyz: what the liquid the eye is in soaks up; w: exposure
	ivec4	output_i;		// tone curve, filter passes, frames kept while moving, there is history
	vec4	output_f;		// saturation, contrast, bloom, nothing has changed since last frame
	ivec4	frame_has;		// smooth normals, where things were last frame, which of each pair of images is this frame's, anti-aliasing
	ivec4	size;			// xy: of the picture being traced; z: simulated bodies of liquid
	vec4	water_rect[24];	// each body's extent: min x, min y, max x, max y
	vec4	water_at[24];	// x: the height of its surface; y: a material that carries its maps, or less than 0: none of it is seen from above; z: its wave picture; w: its caustic picture
	vec4	water_wave[24];	// where its pictures lie: the material's wave_rect
	vec4	water_lamp[24 * 16];	// xyz: where the light on it comes from, for the patterns its waves throw, over each of four by four parts of its extent: see Lamp
	ivec4	out_size;		// xy: of the finished picture, the size of the view
	vec4	open_origin;	// motion blur: the eye as the shutter opened; w: there is blur
	vec4	open_forward;
	vec4	open_right;
	vec4	open_up;
	ivec4	held;			// x: first triangle of the frame carried by the eye (the weapon in hand); y: how many; z: reflections are followed where they appear to be
	vec4	painted;		// x: what a metal painted dark reflects, see pt_view_t's metal_colour; y: the most over white that adds to the glow, 0 = no limit; z: points along a view ray where the air's light is looked for; w: frames of it kept while things change
	vec4	liquid;			// x: how far from level the waves of simulated liquids reach; y: how readily what was gathered is let go where the light has changed, see change.comp; z: how much liquids scatter the light in them; w: how wet their banks show
	ivec4	table_at3;		// in indices: x: for each of the map's lights, the one taken in its stead when it is not kept; y: the cells' lists of lights again, in order of light
} fr;

// the map and what moves, each as three corners per triangle, what goes with
// each triangle, and its materials
layout(std430, set = 0, binding = 2) readonly buffer WorldCorners { float v[]; } world_corners;
layout(std430, set = 0, binding = 3) readonly buffer WorldTris { Tri t[]; } world_tris;
layout(std430, set = 0, binding = 4) readonly buffer WorldMaterials { Material m[]; } world_materials;
layout(std430, set = 0, binding = 5) readonly buffer FrameCorners { float v[]; } frame_corners;
layout(std430, set = 0, binding = 6) readonly buffer FrameTris { Tri t[]; } frame_tris;
layout(std430, set = 0, binding = 7) readonly buffer FrameMaterials { Material m[]; } frame_materials;

layout(set = 0, binding = 8) uniform texture2D textures[];
layout(set = 0, binding = 9) uniform sampler nearest_sampler;
layout(set = 0, binding = 10) uniform sampler smooth_sampler;

layout(std430, set = 0, binding = 11) readonly buffer WorldLights { Light l[]; } world_lights;
layout(std430, set = 0, binding = 12) readonly buffer FrameLights { Light l[]; } frame_lights;
// 256 light styles, then the tables the frame block says where to find
layout(std430, set = 0, binding = 13) readonly buffer Tables { float v[]; } tables;
layout(std430, set = 0, binding = 14) readonly buffer Indices { uint v[]; } indices;
layout(std430, set = 0, binding = 15) readonly buffer FrameNormals { float v[]; } frame_normals;
layout(std430, set = 0, binding = 16) readonly buffer FramePrev { float v[]; } frame_prev;

// Per pixel, at the size the picture is traced at. Where there are two of
// something, one is this frame's and the other the last one's, in turn.
layout(set = 0, binding = 17, rgba16f) uniform image2D img_surface[2];	// shading normal, distance (negative: none)
layout(set = 0, binding = 18, rgba32f) uniform image2D img_seen;		// where to look for it in the last frame; roughness
layout(set = 0, binding = 19, rgba16f) uniform image2D img_albedo[2];	// what the diffuse and specular light are multiplied by; [1].a: the surface's roughness, as img_seen has it but cheaper to read
layout(set = 0, binding = 20, rgba16f) uniform image2D img_noisy[3];	// this frame's diffuse, specular and layer light; a: how much the pixel's paths disagreed, as the variance of their luminance
layout(set = 0, binding = 21, rgba16f) uniform image2D img_extra;		// light that needs no filtering
layout(set = 0, binding = 22, rgba16f) uniform image2D img_kept[6];		// the three gathered over time; a: frames
layout(set = 0, binding = 23, rgba16f) uniform image2D img_filter[6];	// the three being filtered
layout(set = 0, binding = 24, rgba8) uniform image2D img_picture;
layout(std430, set = 0, binding = 25) buffer Meter { uint v[]; } meter;	// sums for the exposure
layout(set = 0, binding = 26, rgba16f) uniform image2D img_hdr;			// the picture put together, exposed, before grading
layout(set = 0, binding = 27, rgba16f) uniform image2D img_bloom[2];	// its glow, at half size in the corner of each
// these two are the size of the view, which may be larger than what is traced
layout(set = 0, binding = 28, rgba16f) uniform image2D img_steady[2];	// the finished picture gathered over frames; a: how much stands behind it
layout(set = 0, binding = 29, rgba16f) uniform image2D img_graded;		// the picture graded for the screen, as traced
// Where a reflection appears to be, which is where its history is looked
// up: a mirror shows something else as soon as the eye moves, so what the
// surface reflected last frame is not where the surface was.
layout(set = 0, binding = 30, rgba32f) uniform image2D img_mirror[2];	// xyz: what a mirror-like solid surface reflects appears to sit here; w: the surface's roughness, plus 2 where there is no such reflection to follow
layout(set = 0, binding = 31, rgba32f) uniform image2D img_over;		// xyz: the same for what the layers in front reflect; w: there are layers
// How much each channel's light has varied, gathered over frames the way the
// light is: the filter smooths as far as the noise measured warrants and no
// further. The mean and the mean square of the luminance, the frame's and the
// last one's, in turn, like img_kept.
layout(set = 0, binding = 32, rg32f) uniform image2D img_moments[6];
// how much the frame's lights put on the pixel, see FrameLightLevel, by parity
layout(set = 0, binding = 33, r32f) uniform image2D img_flash[2];
// Where the light has changed, in blocks of 8 pixels: [0] to [2] each
// channel's sums over a block, [3].x how far what was gathered there is to
// be let go, 0 to 1, and .y how far apart the sums were. See change.comp.
layout(set = 0, binding = 34, rgba32f) uniform image2D img_change[4];
// For export only (the view's planes, see pt.h's PT_PLANE_): [0] what the
// surface gives off, and how metallic it is; [1] its material, its triangle,
// and 1 if that is of the frame rather than the map; [2], [3] what every
// light would put on it with nothing in the way, through the matte part and
// through the shine. See DirectEveryLight.
layout(set = 0, binding = 35, rgba32f) uniform image2D img_export[4];

// how far what was gathered at a pixel is to be let go: the blocks' values,
// smoothly from one block to the next
float Changed(ivec2 pixel)
{
	const ivec2 blocks = (fr.size.xy + 7) / 8;
	const vec2 at = (vec2(pixel) + 0.5) / 8.0 - 0.5;
	const ivec2 b0 = ivec2(floor(at));
	const vec2 a = at - vec2(b0);
	const ivec2 lo = clamp(b0, ivec2(0), blocks - 1), hi = clamp(b0 + 1, ivec2(0), blocks - 1);
	return mix(mix(imageLoad(img_change[3], lo).x, imageLoad(img_change[3], ivec2(hi.x, lo.y)).x, a.x),
		mix(imageLoad(img_change[3], ivec2(lo.x, hi.y)).x, imageLoad(img_change[3], hi).x, a.x), a.y);
}

float Luminance(vec3 c)
{
	return dot(c, vec3(0.2126, 0.7152, 0.0722));
}

// How unsure the light gathered in channel k of pixel q still is: the
// variance of its mean, from the moments of its luminance and how many
// frames stand behind it. With little history that cannot be measured and
// is taken to be large.
float VarianceOf(vec2 moments, float frames)
{
	frames = max(frames, 1.0);
	float var = max(moments.y - moments.x * moments.x, 0.0) / frames;
	if (frames < 4.0)
		var = max(var, moments.x * moments.x * 0.25 + 0.01);
	return var;
}

// the same for channel k of pixel q, given how many frames stand behind the
// channel (a channel with none of its own goes by the surface's)
float GatheredVariance(int k, ivec2 q, float frames)
{
	const int now = fr.frame_has.z;
	if (frames <= 0.0)
	{
		frames = imageLoad(img_kept[now * 3], q).a;
		// the air's light, where it has the layer channel to itself, is
		// kept for fewer frames while things change: see temporal.comp
		if (k == 2 && fr.output_f.w == 0.0 && fr.settings_f.w > 0.0)
			frames = min(frames, max(fr.painted.w, 1.0));
	}
	return VarianceOf(imageLoad(img_moments[now * 3 + k], q).xy, frames);
}

float MaxComponent(vec3 c)
{
	return max(c.r, max(c.g, c.b));
}

vec3 ToLinear(vec3 c)
{
	return pow(c, vec3(2.2));
}

// black, blue, green, yellow and red at 0 to 4, cyan between blue and green
vec3 CountColour(float at)
{
	const vec3 ramp[6] = vec3[6](vec3(0.0, 0.0, 0.0), vec3(0.0, 0.0, 1.0), vec3(0.0, 1.0, 1.0), vec3(0.0, 1.0, 0.0),
		vec3(1.0, 1.0, 0.0), vec3(1.0, 0.0, 0.0));
	// cyan is a stop of its own, half way from 1 to 2
	at = clamp(at, 0.0, 4.0);
	const float stop = at < 1.0 ? at : at < 2.0 ? 1.0 + (at - 1.0) * 2.0 : at + 1.0;
	const int below = min(int(stop), 4);
	return mix(ramp[below], ramp[below + 1], stop - float(below));
}

// the colour that stands for a number of bounces, see PT_VIEW_BOUNCES
vec3 BounceColour(float bounces)
{
	return CountColour(bounces);
}

// and for a number of rays, see PT_VIEW_COST
vec3 CostColour(float rays)
{
	// a colour for each doubling from COST_BLUE up, and a fade to black below it
	const float at = rays / COST_BLUE;
	return CountColour(at < 1.0 ? at : 1.0 + log2(at));
}

#ifdef TRACING

// ----------------------------------------------------------------- chance

uint rng_state;

uint RandBits()
{
	// PCG hash step
	rng_state = rng_state * 747796405u + 2891336453u;
	const uint w = ((rng_state >> ((rng_state >> 28u) + 4u)) ^ rng_state) * 277803737u;
	return (w >> 22u) ^ w;
}

// [0, 1)
float Rand()
{
	return float(RandBits() >> 8) * (1.0 / 16777216.0);
}

uint Hash(uint a, uint b)
{
	uint h = a * 0x9E3779B1u ^ (b + 0x7F4A7C15u + (a << 6) + (a >> 2));
	h ^= h >> 16; h *= 0x85EBCA6Bu;
	h ^= h >> 13; h *= 0xC2B2AE35u;
	h ^= h >> 16;
	return h;
}

void Basis(vec3 n, out vec3 t, out vec3 b)
{
	const float s = n.z >= 0.0 ? 1.0 : -1.0;
	const float a = -1.0 / (s + n.z);
	const float c = n.x * n.y * a;
	t = vec3(1.0 + s * n.x * n.x * a, s * c, -s * n.x);
	b = vec3(c, s + n.y * n.y * a, -n.y);
}

// the first of count sorted values in tables, from base on, greater than x
int UpperBound(int base, int count, float x)
{
	int lo = 0, hi = count;
	while (lo < hi)
	{
		const int mid = (lo + hi) >> 1;
		if (tables.v[base + mid] <= x)
			lo = mid + 1;
		else
			hi = mid;
	}
	return lo;
}

// Where in a cell's list light li is, or -1 if it is not there. sorted is
// where the list begins in indices as it is kept for this: in order of light,
// each with its place in the list as the tables have it in its low five bits.
int PlaceInCell(int sorted, int count, uint li)
{
	// the first that is of a later light
	int lo = 0, hi = count;
	while (lo < hi)
	{
		const int mid = (lo + hi) >> 1;
		if ((indices.v[sorted + mid] >> 5) <= li)
			lo = mid + 1;
		else
			hi = mid;
	}
	if (lo == 0)
		return -1;
	const uint before = indices.v[sorted + lo - 1];
	return (before >> 5) == li ? int(before & 31u) : -1;
}

// ------------------------------------------------------------ the scene

vec3 Corner(bool moving, int tri, int k)
{
	const int i = tri * 9 + k * 3;
	return moving ? vec3(frame_corners.v[i], frame_corners.v[i + 1], frame_corners.v[i + 2])
		: vec3(world_corners.v[i], world_corners.v[i + 1], world_corners.v[i + 2]);
}

Material MaterialOf(bool moving, uint index)
{
	if (moving)
		return frame_materials.m[index];
	return world_materials.m[index];
}

vec4 Texel(int slot, vec2 uv, bool smooth_it)
{
	if (smooth_it)
		return textureLod(sampler2D(textures[nonuniformEXT(slot)], smooth_sampler), uv, 0.0);
	return textureLod(sampler2D(textures[nonuniformEXT(slot)], nearest_sampler), uv, 0.0);
}

// about the sky's axis, by the angle whose sine is s
vec3 Turn(vec3 v, float s)
{
	const vec3 axis = fr.sky_turn.xyz;
	return v * fr.sky_misc.x + cross(axis, v) * s + axis * (dot(axis, v) * (1.0 - fr.sky_misc.x));
}

// The sky is a cube of six pictures around the eye. On the face along axis
// a, with b and c the next two axes in turn, a direction d lands at
//   u = (d[b] / |d[a]| + 1) / 2,  v = (d[c] / |d[a]| + 1) / 2
// d is in the sky's own frame, before any turning.
vec3 SkyAt(vec3 d)
{
	const vec3 a = abs(d);
	int axis = 0;
	if (a.y > a.x) axis = 1;
	if (a.z > (axis == 1 ? a.y : a.x)) axis = 2;
	const int face = axis * 2 + (d[axis] < 0.0 ? 1 : 0);
	const int tex = face < 4 ? fr.sky_a[face] : fr.sky_b[face - 4];
	if (tex < 0 || a[axis] <= 0.0)
		return vec3(0.0);
	const vec2 uv = vec2(d[(axis + 1) % 3], d[(axis + 2) % 3]) / a[axis] * 0.5 + 0.5;
	const ivec2 size = textureSize(sampler2D(textures[nonuniformEXT(tex)], nearest_sampler), 0);
	const ivec2 at = clamp(ivec2(uv * vec2(size)), ivec2(0), size - 1);
	return ToLinear(texelFetch(sampler2D(textures[nonuniformEXT(tex)], nearest_sampler), at, 0).rgb) * fr.sky_misc.y;
}

vec3 Sky(vec3 world_dir)
{
	return SkyAt(Turn(world_dir, -fr.sky_turn.w));
}

struct Hit
{
	bool	moving;		// part of the frame, not the map
	int		tri;
	vec2	bary;		// weights of corners 1 and 2
	float	t;
};

// The instances of the scene carry one of these masks: everything but what
// the eye carries (the weapon in hand) has MASK_SCENE, that has MASK_HELD.
const uint MASK_SCENE = 1u;
const uint MASK_HELD = 2u;
const uint MASK_ALL = 0xffu;

// rays traced for this pixel so far, for VIEW_COST
int rays_traced = 0;

// the nearest thing along the ray among the instances the mask lets through,
// taking every triangle as it comes; glass is only met from its front
bool Nearest(vec3 origin, vec3 dir, float tmin, float tmax, uint mask, out Hit hit)
{
	rayQueryEXT query;
	rayQueryInitializeEXT(query, scene, gl_RayFlagsOpaqueEXT | gl_RayFlagsCullBackFacingTrianglesEXT,
		mask, origin, tmin, dir, tmax);
	while (rayQueryProceedEXT(query))
		;
	if (rayQueryGetIntersectionTypeEXT(query, true) == gl_RayQueryCommittedIntersectionNoneEXT)
		return false;

	// instances: 0 the map, 1 its glass, 2 what moves, 3 its glass, 4 what the eye carries
	const int instance = rayQueryGetIntersectionInstanceCustomIndexEXT(query, true);
	hit.moving = instance >= 2;
	hit.tri = rayQueryGetIntersectionPrimitiveIndexEXT(query, true)
		+ (instance == 4 ? fr.held.x : ((instance & 1) != 0 ? fr.bases[instance] : 0));
	hit.bary = rayQueryGetIntersectionBarycentricsEXT(query, true);
	hit.t = rayQueryGetIntersectionTEXT(query, true);
	return true;
}

bool Nearest(vec3 origin, vec3 dir, float tmin, float tmax, out Hit hit)
{
	return Nearest(origin, dir, tmin, tmax, MASK_ALL, hit);
}

Tri TriOf(Hit hit)
{
	if (hit.moving)
		return frame_tris.t[hit.tri];
	return world_tris.t[hit.tri];
}

vec2 TexCoord(Tri tri, vec2 bary)
{
	return tri.uv0 * (1.0 - bary.x - bary.y) + tri.uv1 * bary.x + tri.uv2 * bary.y;
}

bool IsHole(Material mat, Tri tri, vec2 bary)
{
	if ((mat.flags & MAT_ALPHA_TEST) == 0u || mat.texture < 0)
		return false;
	return Texel(mat.texture, TexCoord(tri, bary), false).a < 0.5;
}

float StoredHeight(ivec2 at, int map)
{
	const vec2 h = texelFetch(sampler2D(textures[nonuniformEXT(map)], nearest_sampler), at, 0).ba;
	return h.x * 65280.0 + h.y * 255.0;
}

// how far above its level a simulated liquid stands at xy: map is its wave
// picture, of this size, lying at rect
float WaveHeight(int map, vec4 rect, ivec2 size, vec2 xy)
{
	// between the middles of the cells, and level with the outermost beyond them
	const vec2 f = clamp((xy - rect.xy) * rect.zw * vec2(size) - 0.5, vec2(0.0), vec2(size - 1));
	const ivec2 at = min(ivec2(f), size - 2);
	const vec2 a = f - vec2(at);
	const float h = mix(mix(StoredHeight(at, map), StoredHeight(at + ivec2(1, 0), map), a.x),
		mix(StoredHeight(at + ivec2(0, 1), map), StoredHeight(at + ivec2(1, 1), map), a.x), a.y);
	return (h * (1.0 / 65535.0) - 0.5) * 16.0 * fr.settings_f.z;
}

// Where the ray first crosses the surface of a simulated liquid before limit.
// The surface is not the level triangles the map has for it but stands where
// the waves do, above and below them: the ray is followed through the band
// the waves keep to until it is on the other side of the surface. Gives how
// far along that is, the side the ray came from (1 above, -1 below), the
// level of the liquid and its wave picture.
bool WaveCross(vec3 origin, vec3 dir, float tmin, float limit, out float where, out float side, out float level, out int map)
{
	bool found = false;
	where = 0.0;
	side = 1.0;
	level = 0.0;
	map = -1;
	const vec3 inv = 1.0 / mix(dir, vec3(1.0e-8), lessThan(abs(dir), vec3(1.0e-8)));
	const float reach = fr.liquid.x * fr.settings_f.z + 0.1;
	limit = min(limit, 1.0e7);
	for (int i = 0; i < fr.size.z; i++)
	{
		// the part of the ray within the waves' reach of the level, over the body
		const float z = fr.water_at[i].x;
		const vec3 a = (vec3(fr.water_rect[i].xy, z - reach) - origin) * inv, b = (vec3(fr.water_rect[i].zw, z + reach) - origin) * inv;
		const vec3 lo = min(a, b), hi = max(a, b);
		const float t0 = max(tmin, max(lo.x, max(lo.y, lo.z))), t1 = min(limit, min(hi.x, min(hi.y, hi.z)));
		if (t0 >= t1)
			continue;
		const int waves = int(fr.water_at[i].z), caustics = int(fr.water_at[i].w);
		const vec4 rect = fr.water_wave[i];
		if (waves < 0)
			continue;
		const ivec2 size = textureSize(sampler2D(textures[nonuniformEXT(waves)], nearest_sampler), 0);
		if (size.x < 2 || size.y < 2)
			continue;

#define ABOVE(t) (origin.z + dir.z * (t) - z - WaveHeight(waves, rect, size, origin.xy + dir.xy * (t)))
		// Far from the surface the steps are as long as its slope allows,
		// taken to be no steeper than one in one; near it half a cell. A ray
		// that skims it for long gets longer steps as it goes, so as to end.
		const float cell = 1.0 / (rect.z * float(size.x));
		const float across = length(dir.xy);
		const float closing = 1.0 / (abs(dir.z) + across);
		float least = 0.5 * cell / max(across, 1.0e-6);

		float ta = t0, fa = ABOVE(t0);
		// a ray that leaves the surface starts on it, and a little further
		// along shows which side it left on
		if (abs(fa) < 0.05)
			fa = ABOVE(min(t0 + 0.1, t1));
		for (int k = 0; ta < t1; k++)
		{
			if (k >= 64)
				least *= 1.06;
			const float tb = min(ta + max(abs(fa) * closing, least), t1);
			const float fb = ABOVE(tb);
			if ((fa < 0.0) != (fb < 0.0))
			{
				// the caustic picture also says where there is liquid at all
				const ivec2 cell_at = clamp(ivec2(floor((origin.xy + dir.xy * (0.5 * (ta + tb)) - rect.xy) * rect.zw * vec2(size))), ivec2(0), size - 1);
				if (caustics < 0 || texelFetch(sampler2D(textures[nonuniformEXT(caustics)], nearest_sampler), cell_at, 0).a > 0.5)
				{
					float on = ta, past = tb;
					for (int halving = 0; halving < 10; halving++)
					{
						const float mid = 0.5 * (on + past);
						if ((ABOVE(mid) < 0.0) == (fa < 0.0))
							on = mid;
						else
							past = mid;
					}
					where = on;
					side = fa < 0.0 ? -1.0 : 1.0;
					level = z;
					map = waves;
					limit = on;
					found = true;
					break;
				}
			}
			ta = tb;
			fa = fb;
		}
#undef ABOVE
	}
	return found;
}

// A ray that leaves the surface of a simulated liquid where it stands below
// its level is behind the level sheet, and must not meet that from the back.
bool BehindSheet(Hit hit, vec3 dir)
{
	if (hit.moving || fr.settings_f.z <= 0.0)
		return false;
	const vec3 p0 = Corner(false, hit.tri, 0);
	return dot(cross(Corner(false, hit.tri, 1) - p0, Corner(false, hit.tri, 2) - p0), dir) > 0.0;
}

// The nearest thing a path meets: not what only casts shadows (for the
// eye), not the holes in a grating, and, for light finding its way through
// (cross), a see-through surface only as often as it is opaque.
bool Closest(vec3 origin, vec3 dir, inout float tmin, bool camera, bool cross_through, uint mask, bool waves, out Hit hit, out Material mat)
{
	rays_traced++;
	const bool swell = waves && fr.size.z > 0 && fr.settings_f.z > 0.0;
	for (int skips = 0; ; skips++)
	{
		// One search a turn, so that there is one of them here and no more:
		// each place the shader searches from costs every ray. First for
		// the nearest thing, again from behind it if that is the level
		// triangle of a simulated liquid, which is met where its waves stand
		// (see WaveCross); then, if the ray crosses those first, straight
		// down or up through the crossing for the triangle that says there is
		// liquid there and what it is made of.
		bool found = false, probing = false;
		float from = tmin, where = 0.0, side = 1.0, level = 0.0;
		int map = -1;
		for (int turn = 0; turn < 12; turn++)
		{
			Hit h;
			const bool got = Nearest(probing ? vec3(origin.xy + dir.xy * where, level + side * 1.25) : origin,
				probing ? vec3(0.0, 0.0, -side) : dir, from, probing ? 2.5 : 1.0e30, probing ? MASK_SCENE : mask, h);
			if (!probing)
			{
				if (got)
					mat = MaterialOf(h.moving, TriOf(h).material);
				if (got && swell && (mat.bits & BIT_SWELL) != 0u && turn < 8)
				{
					from = h.t + 0.01;
					continue;
				}
				found = got;
				hit = h;
				if (!swell || !WaveCross(origin, dir, tmin, got ? h.t : 1.0e30, where, side, level, map))
					break;
				probing = true;
				from = 0.0;
			}
			else
			{
				if (!got)
					break;
				from = h.t + 1.0e-3;
				if (h.moving)
					continue;
				const Material there = world_materials.m[world_tris.t[h.tri].material];
				if (there.wave_map != map)
					continue;
				const vec3 p0 = Corner(false, h.tri, 0);
				if (cross(Corner(false, h.tri, 1) - p0, Corner(false, h.tri, 2) - p0).z * side > 0.0)
				{
					hit = h;
					hit.t = where;
					mat = there;
					found = true;
					break;
				}
			}
		}
		if (!found)
			return false;
		const Tri tri = TriOf(hit);
		const bool skip = skips < 32 && (
			(camera && (mat.flags & MAT_CAMERA_INVISIBLE) != 0u) ||
			IsHole(mat, tri, hit.bary) ||
			(cross_through && mat.alpha < 1.0 && Rand() >= mat.alpha) ||
			(fr.bases.x == VIEW_FURNACE && (mat.flags & MAT_BLACK) != 0u) ||
			(!waves && (mat.bits & BIT_SWELL) != 0u && BehindSheet(hit, dir)));
		if (!skip)
			return true;
		tmin = hit.t + 0.01;
	}
}

// for light finding its way about: to that a simulated liquid is the level
// sheet the map has for it, which costs every such ray less to look for
bool Closest(vec3 origin, vec3 dir, inout float tmin, bool camera, bool cross_through, out Hit hit, out Material mat)
{
	return Closest(origin, dir, tmin, camera, cross_through, MASK_ALL, false, hit, mat);
}

// Ripples too fine for a simulated liquid's cells to hold, where it is as
// ruffled as can be: a few trains of small waves, each running its own way
// at the speed waves of its length do. How sharply they curve the surface
// at p: along x, along y, and across the two.
vec3 RippleCurve(vec2 p)
{
	vec3 c = vec3(0.0);
	for (int i = 0; i < 6; i++)
	{
		const float turn = float(i) * 2.399 + 0.5;
		const vec2 d = vec2(cos(turn), sin(turn));
		const float k = 0.28 + 0.11 * float(i);		// from 22 units long down to 7
		// a wave whose slope is at most RIPPLE_SLOPE curves by that times k
		const float bend = sin(dot(d, p) * k - sqrt(386.0 * k) * fr.sky_misc.w + float(i) * 1.7) * (RIPPLE_SLOPE * k);
		c -= vec3(d.x * d.x, d.y * d.y, d.x * d.y) * bend;
	}
	return c;
}

// How much the waves of a simulated liquid brighten light that goes
// through its surface at q, or dim it: where the surface bulges it gathers
// the light, as a lens does, and where it is hollow it spreads it. map is
// its picture of that, lying at rect; reach is how far the light goes on
// from there, times how much the surface turns it for its slope, which is
// 1 - 1 / the liquid's index. wet is whether there is liquid at q at all.
float Caustic(int map, vec4 rect, vec3 q, float reach, out bool wet)
{
	const vec2 at = (q.xy - rect.xy) * rect.zw;
	wet = at.x >= 0.0 && at.y >= 0.0 && at.x <= 1.0 && at.y <= 1.0;
	if (!wet)
		return 1.0;
	const vec4 v = Texel(map, at, true);
	wet = v.a > 0.3;
	// how sharply it curves along x and along y, and how ruffled it is: see pt_water_caustics
	const vec2 s = (v.rg * 255.0 - 128.0) * (1.0 / 127.0);
	const vec3 c = (vec3(s * abs(s) * 0.125, 0.0) + RippleCurve(q.xy) * (v.b * 4.0)) * (reach * fr.settings_f.z);
	return clamp(1.0 / max(abs((1.0 + c.x) * (1.0 + c.y) - c.z * c.z), 0.01), 0.2, CAUSTIC_MAX);
}

// Light bends where it goes into a liquid. Of a path with one end in the
// liquid and the other out of it, whose surface lies level at z: where it
// crosses, which is nearer over the end in the liquid than a straight line
// between the two would.
vec3 BentCrossing(vec3 in_liquid, vec3 out_of_it, float z)
{
	const float down = z - in_liquid.z, up = out_of_it.z - z;
	const vec2 across = out_of_it.xy - in_liquid.xy;
	const float len = length(across);
	if (len < 1.0e-4)
		return vec3(in_liquid.xy, z);
	// the sines of the two angles at the surface are as the liquid's index:
	// from where that puts it for small angles, and nearer each step
	const float straight = len * down / (down + up);
	float x = len * down / (down + WATER_INDEX * up);
	for (int i = 0; i < 3; i++)
	{
		const float y = len - x;
		const float a = inversesqrt(x * x + down * down), b = inversesqrt(y * y + up * up);
		x = clamp(x - (WATER_INDEX * x * a - y * b) / (WATER_INDEX * down * down * a * a * a + up * up * b * b * b), 0.0, straight);
	}
	return vec3(in_liquid.xy + across * (x / len), z);
}

// Whether the way from p to target crosses the surface of a simulated
// liquid, where it bends: gives where it crosses, and how much of the light
// gets through there.
bool Bend(vec3 p, vec3 target, out vec3 q, out float through)
{
	q = vec3(0.0);
	through = 1.0;
	for (int i = 0; i < fr.size.z; i++)
	{
		const float z = fr.water_at[i].x;
		if ((p.z - z) * (target.z - z) >= 0.0)
			continue;
		const int marks = int(fr.water_at[i].w);
		if (marks < 0)
			continue;
		const bool rising = target.z > z;
		const vec3 above = rising ? target : p;
		const vec3 at = BentCrossing(rising ? p : target, above, z);
		// the picture of its curves also says where there is liquid at all
		const vec2 uv = (at.xy - fr.water_wave[i].xy) * fr.water_wave[i].zw;
		if (uv.x < 0.0 || uv.y < 0.0 || uv.x > 1.0 || uv.y > 1.0 || Texel(marks, uv, false).a < 0.5)
			continue;
		// less of it gets in at a glancing angle, and what is painted on the
		// liquid stops its share
		const float m = 1.0 - (above.z - z) / distance(at, above);
		through = 0.98 - 0.98 * m * m * m * m * m;
		if (fr.water_at[i].y >= 0.0)
			through *= 1.0 - world_materials.m[int(fr.water_at[i].y)].alpha;
		q = at;
		return true;
	}
	return false;
}

// Whether light from target reaches a point: 1, or 0 if something is in the
// way. With bent, the way is not straight but by q, where it crosses the
// surface of a liquid: see Bend.
float Visible(vec3 p, vec3 target, bool bent, vec3 q)
{
	rays_traced += bent ? 2 : 1;
	vec3 o = p, d = (bent ? q : target) - p;
	float tmin = 0.0;
	for (int skips = 0; skips < 16; skips++)
	{
		Hit hit;
		if (!Nearest(o, d, tmin, 0.999, hit))
		{
			if (!bent)
				return 1.0;
			// nothing in the way as far as the surface: now from there on
			bent = false;
			o = q;
			d = target - q;
			tmin = 1.0e-3;
			continue;
		}
		const Tri tri = TriOf(hit);
		const Material mat = MaterialOf(hit.moving, tri.material);
		if (!IsHole(mat, tri, hit.bary) && (mat.alpha >= 1.0 || Rand() < mat.alpha))
			return 0.0;
		tmin = hit.t + 1.0e-4;
	}
	return 0.0;
}

float Visible(vec3 p, vec3 target)
{
	return Visible(p, target, false, vec3(0.0));
}

// --------------------------------------------------------------- surfaces

struct Surface
{
	vec3	p;
	vec3	ng;			// geometric normal, on the side the ray came from
	vec3	n;			// shading normal
	vec3	wo;			// unit, towards where the ray came from
	vec3	tri_n;		// the triangle's own normal, whichever side was met
	bool	front;		// seen from the triangle's counter clockwise side
	vec3	colour;		// the texture's colour here
	vec3	glow;		// the emission map's, where the material has one
	vec3	kd;			// diffuse reflectance
	vec3	f0;			// specular reflectance head on
	float	roughness;
	float	metallic;	// here: the material's, or its texture's
	float	alpha;		// GGX width, roughness squared
	bool	light_sampled_spec;
	bool	medium;		// not a surface at all but a point in the air: no facing, scatters evenly
	float	froth;		// how much of a liquid's surface froth covers here; it is in colour and mat.alpha already
	Material mat;		// after animation
};

// lumps of about one unit across, 0 to 1, the same wherever it is asked
float Lumps(vec2 p)
{
	const vec2 f = floor(p);
	vec2 a = p - f;
	const ivec2 i = ivec2(f);
	a = a * a * (3.0 - 2.0 * a);
	const float c00 = float(Hash(uint(i.x), uint(i.y)) >> 8), c10 = float(Hash(uint(i.x + 1), uint(i.y)) >> 8);
	const float c01 = float(Hash(uint(i.x), uint(i.y + 1)) >> 8), c11 = float(Hash(uint(i.x + 1), uint(i.y + 1)) >> 8);
	return mix(mix(c00, c10, a.x), mix(c01, c11, a.x), a.y) * (1.0 / 16777216.0);
}

// Bubbles packed together, about one unit across: 1 on the walls between
// them, falling to 0 in their middles
float Bubbles(vec2 p)
{
	const vec2 cell = floor(p);
	const vec2 f = p - cell;
	const ivec2 c = ivec2(cell);
	float d1 = 8.0, d2 = 8.0;
	for (int j = -1; j <= 1; j++)
	{
		for (int i = -1; i <= 1; i++)
		{
			const uint h = Hash(uint(c.x + i), uint(c.y + j));
			const vec2 o = vec2(float(i) + float(h & 0xffffu) * (1.0 / 65535.0), float(j) + float(h >> 16) * (1.0 / 65535.0)) - f;
			const float d = dot(o, o);
			if (d < d1)
			{
				d2 = d1;
				d1 = d;
			}
			else if (d < d2)
				d2 = d;
		}
	}
	return 1.0 - clamp((sqrt(d2) - sqrt(d1)) * 2.2, 0.0, 1.0);
}

// froth's own shape: large bubbles and small, most where their walls meet
float FrothPattern(vec2 p)
{
	return 0.6 * Bubbles(p * 0.11) + 0.4 * Bubbles(p * 0.37);
}

const float FROTH_SLIDE = 1.6;	// seconds the pattern rides on the liquid before it starts again

// How much of a liquid's surface at p froth covers, 0 to 0.9, given what its
// foam picture says there. Thick and fresh it lies closed; thinner or older
// it opens into rings and strings along the walls of its bubbles. Froth in
// pt/cpu/pt_trace.cpp is the same and is to be kept so.
float Froth(vec2 p, vec4 foam)
{
	if (foam.r <= 0.004)
		return 0.0;
	const vec2 flow = foam.ba * 255.0 - 128.0;
	float pattern;
	if (dot(flow, flow) > 4.0)
	{
		// The liquid carries it along. The pattern is drawn twice, each
		// sliding with the liquid for a while before it starts again, and
		// one fades in as the other fades out.
		const float phase = fract(fr.sky_misc.w * (1.0 / FROTH_SLIDE));
		pattern = mix(FrothPattern(p - flow * (FROTH_SLIDE * (fract(phase + 0.5) - 0.5))),
			FrothPattern(p - flow * (FROTH_SLIDE * (phase - 0.5))), 1.0 - abs(2.0 * phase - 1.0));
	}
	else
		pattern = FrothPattern(p);
	return clamp((pattern - 1.0 + foam.r * (1.3 - 0.45 * foam.g)) * 3.0, 0.0, 1.0) * 0.9;
}

// What a metal reflects, worked out from the colour it was painted
// (MAT_METAL_PAINTED): pt_material_metal_colour in pt/material/pt_material.h,
// which says why, written again here. The two are to be kept the same.
const float METAL_PAINTED = 0.012;
const float METAL_HUE_KEPT = 0.6;

vec3 MetalColour(vec3 c, float level)
{
	if (level <= 0.0)
		return c;
	const float y = max(Luminance(c), 1.0e-6);
	const float bright = max(level * sqrt(y * (1.0 / METAL_PAINTED)), y);
	const float keep = (METAL_HUE_KEPT + (1.0 - METAL_HUE_KEPT) * y / bright) * min(MaxComponent(c) * (1.0 / 0.004), 1.0);
	const vec3 tinted = (vec3(1.0) + (c / y - vec3(1.0)) * keep) * bright;
	return tinted / max(MaxComponent(tinted), 1.0);
}

struct Lit
{
	vec3	diffuse;
	vec3	specular;
};

// the material showing at this step of its animation
Material Animated(Material mat, bool moving)
{
	if (moving || mat.anim_length <= 1)
		return mat;
	for (int i = fr.counts.w % mat.anim_length; i > 0 && mat.anim_next >= 0; i--)
		mat = world_materials.m[mat.anim_next];
	return mat;
}

// Epic's fit to the specular lobe's total reflectance (Karis, "Physically
// Based Shading on Mobile")
vec3 SpecularAlbedo(Surface s)
{
	const float nov = max(0.0, dot(s.n, s.wo));
	const float r0 = 1.0 - s.roughness;
	const float r1 = -0.0275 * s.roughness + 0.0425;
	const float r2 = -0.572 * s.roughness + 1.04;
	const float r3 = 0.022 * s.roughness - 0.04;
	const float a004 = min(r0 * r0, exp2(-9.28 * nov)) * r0 + r1;
	const float scale = -1.04 * a004 + r2, bias = 1.04 * a004 + r3;
	return s.f0 * scale + vec3(bias);
}

void MakeSurface(Hit hit, Material base, vec3 origin, vec3 dir, bool smooth_it, out Surface s)
{
	const Tri tri = TriOf(hit);
	const Material mat = Animated(base, hit.moving);
	const vec3 p0 = Corner(hit.moving, hit.tri, 0), p1 = Corner(hit.moving, hit.tri, 1), p2 = Corner(hit.moving, hit.tri, 2);
	const vec3 e1 = p1 - p0, e2 = p2 - p0;
	const vec3 x = cross(e1, e2);
	const float len = length(x);

	s.mat = mat;
	s.medium = false;
	s.froth = 0.0;
	s.p = origin + dir * hit.t;
	s.wo = -dir;
	s.tri_n = len > 0.0 ? x / len : vec3(0.0, 0.0, 1.0);
	s.front = dot(s.tri_n, dir) < 0.0;
	s.ng = s.front ? s.tri_n : -s.tri_n;

	vec2 uv = TexCoord(tri, hit.bary);
	if ((mat.flags & MAT_WARP) != 0u)
	{
		// each coordinate sways with the other, an eighth of a repeat either way
		const float u0 = uv.x;
		uv.x += 0.125 * sin(uv.y * 8.0 + fr.sky_misc.w);
		uv.y += 0.125 * sin(u0 * 8.0 + fr.sky_misc.w);
	}
	uv += mat.scroll * fr.sky_misc.w;

	// a simulated surface: the wave picture holds its slopes and height
	const float wave_strength = fr.settings_f.z;
	const bool simulated = mat.wave_map >= 0 && wave_strength > 0.0;
	vec2 wave = vec2(0.0);
	float wave_height = 0.0;
	if (simulated)
	{
		const vec2 at = (s.p.xy - mat.wave_rect.xy) * mat.wave_rect.zw;
		const vec4 w = Texel(mat.wave_map, at, true);
		wave = (w.rg - 0.5) * wave_strength;
		wave_height = ((w.b * 65280.0 + w.a * 255.0) * (1.0 / 65535.0) - 0.5) * 16.0 * wave_strength;
		// what the texture stands for lies below the surface, so a tilted
		// surface shows it shifted, as through a lens
		uv += wave * 0.5;
	}

	s.colour = mat.texture < 0 ? vec3(1.0) : ToLinear(Texel(mat.texture, uv, smooth_it).rgb);
	s.glow = mat.emission_map >= 0 ? ToLinear(Texel(mat.emission_map, uv, smooth_it).rgb) : vec3(0.0);
	s.roughness = mat.roughness;
	float metallic = mat.emission_per_texel.a;

	vec3 n = s.ng;
	if (hit.moving && fr.frame_has.x != 0)
	{
		const int i = hit.tri * 9;
		const vec3 n0 = vec3(frame_normals.v[i], frame_normals.v[i + 1], frame_normals.v[i + 2]);
		const vec3 n1 = vec3(frame_normals.v[i + 3], frame_normals.v[i + 4], frame_normals.v[i + 5]);
		const vec3 n2 = vec3(frame_normals.v[i + 6], frame_normals.v[i + 7], frame_normals.v[i + 8]);
		const vec3 sum = n0 * (1.0 - hit.bary.x - hit.bary.y) + n1 * hit.bary.x + n2 * hit.bary.y;
		if (dot(sum, sum) > 1.0e-8)
		{
			n = normalize(sum);
			if (!s.front)
				n = -n;
		}
	}
	if (mat.normal_texture >= 0)
	{
		// x, y, z and roughness; the texture's own directions across the triangle
		const vec4 c = Texel(mat.normal_texture, uv, smooth_it);
		const vec2 d1 = tri.uv1 - tri.uv0, d2 = tri.uv2 - tri.uv0;
		const float det = d1.x * d2.y - d2.x * d1.y;
		vec3 tu, tv;
		if (abs(det) > 1.0e-12)
		{
			tu = normalize(e1 * d2.y - e2 * d1.y) * (det < 0.0 ? -1.0 : 1.0);
			tv = normalize(e2 * d1.x - e1 * d2.x) * (det < 0.0 ? -1.0 : 1.0);
		}
		else
			Basis(s.tri_n, tu, tv);
		vec3 t = c.xyz * 2.0 - 1.0;
		if ((mat.flags & MAT_METAL_TEXTURE) != 0u)
		{
			// the third number is metal: z is what is left of the normal's length
			t.z = sqrt(max(1.0 - dot(t.xy, t.xy), 0.01));
			metallic = c.b;
		}
		n = normalize(tu * t.x + tv * t.y + n * t.z);
		s.roughness = c.a;
	}
	if (simulated)
	{
		// the surface is level, facing up or (seen from below) down
		n = normalize(vec3(-wave, 1.0)) * (n.z < 0.0 ? -1.0 : 1.0);
		if (!hit.moving && (mat.bits & BIT_SWELL) != 0u)
		{
			// met where the waves stand (see WaveCross), on the side this
			// triangle faces: it really is tilted as they are
			n = normalize(vec3(-wave, 1.0)) * (s.tri_n.z < 0.0 ? -1.0 : 1.0);
			s.front = true;
			s.ng = n;
		}
		// crests gather the light in the liquid and troughs spread it
		s.colour *= clamp(1.0 + wave_height * 0.35, 0.6, 1.8);
	}
	else if ((mat.flags & MAT_WAVES) != 0u && wave_strength > 0.0)
	{
		// a few crossing ripples, enough to break up a reflection
		const float t = fr.sky_misc.w;
		const float a = sin(s.p.x * 0.071 + t * 1.7) + sin(s.p.y * 0.053 - t * 1.3)
			+ 0.5 * sin((s.p.x + s.p.y) * 0.19 + t * 2.9);
		const float b = sin(s.p.y * 0.067 + t * 1.5) + sin(s.p.x * 0.047 - t * 1.9)
			+ 0.5 * sin((s.p.x - s.p.y) * 0.17 + t * 2.3);
		vec3 t1, t2;
		Basis(n, t1, t2);
		n = normalize(n + t1 * (a * 0.02 * wave_strength) + t2 * (b * 0.02 * wave_strength));
	}
	// a normal tilted away from the viewer is no use for shading
	if (dot(n, s.wo) < 0.02 || dot(n, s.ng) <= 0.0)
		n = s.ng;
	s.n = n;

	s.metallic = metallic;
	if ((mat.flags & MAT_BLACK) != 0u)
	{
		s.kd = vec3(0.0);
		s.f0 = vec3(0.0);
	}
	else
	{
		s.kd = s.colour * (1.0 - metallic);
		s.f0 = vec3(0.04) * (1.0 - metallic)
			+ ((mat.flags & MAT_METAL_PAINTED) != 0u ? MetalColour(s.colour, fr.painted.x) : s.colour) * metallic;

		// A view mode's say over what the surface is made of (pt_view_t's
		// view_mode). What the surface emits is worked out from its colour,
		// which clay and mirror leave alone.
		const int view_mode = fr.bases.x;
		if (view_mode != VIEW_NORMAL)
		{
			if (view_mode == VIEW_CLAY && (mat.alpha >= 1.0 || (mat.flags & MAT_WAVES) != 0u))
			{
				s.kd = vec3(0.5);
				s.f0 = vec3(0.04);
				s.roughness = 1.0;
				s.mat.alpha = 1.0;		// liquids: solid to the eye
			}
			else if (view_mode == VIEW_MIRROR)
				s.roughness = 0.0;
			else if (view_mode == VIEW_FURNACE)
			{
				s.kd = vec3(1.0);
				s.f0 = vec3(0.04);
				s.roughness = 1.0;
				s.mat.alpha = 1.0;		// everything: solid to the eye
				s.colour = vec3(0.0);	// and nothing glows
				s.glow = vec3(0.0);
			}
		}
	}
	s.alpha = max(s.roughness * s.roughness, MIN_ALPHA);
	s.light_sampled_spec = s.roughness >= LIGHT_SAMPLED_ROUGHNESS;
}

// Froth on a simulated liquid, for the eye's own rays, which alone see it.
// It is air and liquid finely mixed, which scatters light every way: a pale
// matt layer over the liquid, which still shows through where the layer is
// thin and still shines a little where it is not. Bubbles do not lie flat.
void AddFroth(inout Surface s)
{
	if (s.mat.foam_map < 0 || s.mat.wave_map < 0 || fr.settings_f.z <= 0.0)
		return;
	const vec4 foam = Texel(s.mat.foam_map, (s.p.xy - s.mat.wave_rect.xy) * s.mat.wave_rect.zw, true);
	const float froth = Froth(s.p.xy, foam);
	if (froth <= 0.0)
		return;

	const vec3 pale = mix(s.colour, vec3(1.0), 0.6) * ((0.55 - 0.2 * foam.g) * (0.7 + 0.3 * Lumps(s.p.xy * 2.3)));
	const float both = s.mat.alpha + froth - s.mat.alpha * froth;
	s.colour = (s.colour * (s.mat.alpha * (1.0 - froth)) + pale * froth) / both;
	s.kd = s.colour * (1.0 - s.metallic);
	s.froth = froth;
	s.mat.alpha = both;
	const vec3 n = normalize(s.n + vec3(Lumps(s.p.xy * 1.9) - 0.5, Lumps(s.p.yx * 1.9 + 31.7) - 0.5, 0.0) * (0.5 * froth));
	if (dot(n, s.wo) >= 0.02 && dot(n, s.ng) > 0.0)
		s.n = n;
	s.glow = vec3(0.0);
	s.mat.flags &= ~(MAT_EMIT_BRIGHT | MAT_BLACK);
	s.mat.bits &= ~BIT_EMISSIVE;
	s.mat.emission = vec4(0.0);
	s.mat.emission_per_texel.rgb = vec3(0.0);
}

// where the light on a simulated liquid comes from, over a point of it
vec3 Lamp(int body, vec2 xy)
{
	const vec4 rect = fr.water_rect[body];
	const vec2 f = clamp((xy - rect.xy) / (rect.zw - rect.xy) * 4.0 - 0.5, vec2(0.0), vec2(3.0));
	const ivec2 at = min(ivec2(f), ivec2(2));
	const vec2 a = f - vec2(at);
	const int k = body * 16 + at.y * 4 + at.x;
	return mix(mix(fr.water_lamp[k].xyz, fr.water_lamp[k + 1].xyz, a.x), mix(fr.water_lamp[k + 4].xyz, fr.water_lamp[k + 5].xyz, a.x), a.y);
}

// What a simulated liquid does to something solid in it or by it, for the
// eye's own rays; under is whether the ray is in a liquid where it met the
// thing.
//
// In the liquid, its waves gather the light that comes down through them
// into bright lines and thin it between. Light is gathered over many
// frames, in which time the waves have moved on and their pattern would
// come to nothing, so it is not in the light as it is traced: it is worked
// out here for the light from where most of the liquid's light comes from,
// and laid over the surface like paint, which nothing blurs.
//
// By it, its banks are wet as far up as it has lately stood and a little
// further: darker, and shiny.
void AddWater(inout Surface s, bool under)
{
	if (fr.settings_f.z <= 0.0)
		return;
	for (int i = 0; i < fr.size.z; i++)
	{
		const float level = fr.water_at[i].x, up = s.p.z - level;
		const vec4 rect = fr.water_rect[i];
		const int waves = int(fr.water_at[i].z), marks = int(fr.water_at[i].w);
		if (up > 12.0 || (!under && up < -9.0) || waves < 0 || marks < 0
			|| s.p.x < rect.x - 8.0 || s.p.y < rect.y - 8.0 || s.p.x > rect.z + 8.0 || s.p.y > rect.w + 8.0)
			continue;

		// how high it has stood, between the cells that have liquid in or by them
		const ivec2 size = textureSize(sampler2D(textures[nonuniformEXT(marks)], nearest_sampler), 0);
		const vec2 f = (s.p.xy - fr.water_wave[i].xy) * fr.water_wave[i].zw * vec2(size) - 0.5;
		const ivec2 at = ivec2(floor(f));
		const vec2 a = f - vec2(at);
		float sum = 0.0, weight = 0.0;
		for (int k = 0; k < 4; k++)
		{
			const ivec2 c = at + ivec2(k & 1, k >> 1);
			if (c.x < 0 || c.y < 0 || c.x >= size.x || c.y >= size.y)
				continue;
			const float v = texelFetch(sampler2D(textures[nonuniformEXT(marks)], nearest_sampler), c, 0).a;
			const float w = ((k & 1) != 0 ? a.x : 1.0 - a.x) * ((k >> 1) != 0 ? a.y : 1.0 - a.y);
			if (v > 0.5)
			{
				sum += v * w;
				weight += w;
			}
		}
		if (weight <= 0.0)
			continue;

		if (up < WaveHeight(waves, fr.water_wave[i], size, s.p.xy))
		{
			// in it, if the eye's ray is; something under its bed is not
			const vec3 lamp = Lamp(i, s.p.xy);
			if (!under || lamp.z <= level || up >= 0.0)
				continue;
			const vec3 q = BentCrossing(s.p, lamp, level);
			const float down = distance(q, s.p), from = distance(q, lamp);
			bool wet;
			const float gain = Caustic(marks, fr.water_wave[i], q, (1.0 - 1.0 / WATER_INDEX) * down * from / (down + from), wet);
			// what faces away from the light has none of it
			const float facing = clamp(dot(s.n, q - s.p) / down * 4.0, 0.0, 1.0);
			if (wet)
				s.kd *= mix(1.0, gain, facing);
			return;
		}
		if (fr.liquid.w <= 0.0)
			continue;

		const float mark = (sum / weight * 255.0 - 160.0) * (8.0 / 95.0) * fr.settings_f.z;
		// it soaks a little higher than it stood, and not to a ruled line
		const float edge = mark + 0.75 + 1.5 * Lumps(vec2(s.p.x + s.p.y, s.p.z + s.p.x - s.p.y) * 0.45);
		const float wet = clamp((edge - up) * 1.5, 0.0, 1.0) * min(fr.liquid.w, 1.0);
		if (wet <= 0.0)
			continue;

		// water fills what made the surface rough and pale
		const float dark = 1.0 - 0.45 * wet;
		s.colour *= dark;
		s.kd *= dark;
		s.roughness = min(s.roughness, mix(s.roughness, 0.3, wet));
		s.alpha = max(s.roughness * s.roughness, MIN_ALPHA);
		s.light_sampled_spec = s.roughness >= LIGHT_SAMPLED_ROUGHNESS;
		return;
	}
}

// what the surface gives off; seen is for the eye looking straight at it
vec3 Emitted(Surface s, bool seen)
{
	const Material m = s.mat;
	if (m.emission_map >= 0)
	{
		if ((m.flags & MAT_EMIT_MAPPED) == 0u)
			return m.emission.rgb * s.glow;
		// the material's own light, from where the map says it comes
		return (seen && m.emission.a > 0.0) ? s.glow * m.emission.a : m.emission_per_texel.rgb * s.glow;
	}
	if ((m.flags & MAT_EMIT_BRIGHT) != 0u)
	{
		// the lit parts of a screen or a button: bright texels glow, dark ones do not
		const float t = clamp((MaxComponent(s.colour) - 0.12) * (1.0 / 0.3), 0.0, 1.0);
		return m.emission.rgb * s.colour * (t * t * (3.0 - 2.0 * t));
	}
	return (seen && m.emission.a > 0.0) ? s.colour * m.emission.a : m.emission_per_texel.rgb * s.colour;
}

// the lighting only view: the surface as it would be were its texture white
void WhiteSurface(inout Surface s)
{
	if ((s.mat.flags & MAT_BLACK) != 0u)
		return;
	s.kd = vec3(1.0 - s.metallic);
	s.f0 = vec3(0.04 * (1.0 - s.metallic) + s.metallic);
}

// what a view of one thing known of the surface shows (VIEW_BASE_COLOUR to
// VIEW_GLOW)
vec3 SurfaceChannel(int mode, Surface s)
{
	// but for the colour, shown as the number it is: the display's curve
	// undoes this one
	if (mode == VIEW_BASE_COLOUR)
		return s.colour;
	if (mode == VIEW_NORMALS)
		return ToLinear(clamp(0.5 + 0.5 * s.n, 0.0, 1.0));
	if (mode == VIEW_ROUGHNESS)
		return ToLinear(vec3(clamp(s.roughness, 0.0, 1.0)));
	if (mode == VIEW_METAL)
		return ToLinear(vec3(clamp(s.metallic, 0.0, 1.0)));
	if (mode == VIEW_GLOW && (s.mat.bits & BIT_EMISSIVE) != 0u && s.front)
	{
		const vec3 e = Emitted(s, true);
		return e / max(1.0, MaxComponent(e));
	}
	return vec3(0.0);
}

// ---- GGX microfacet reflection, height correlated Smith shadowing ----

vec3 Fresnel(vec3 f0, float voh)
{
	const float m = 1.0 - max(voh, 0.0);
	const float m5 = m * m * m * m * m;
	return f0 + (vec3(1.0) - f0) * m5;
}

float SmithLambda(float alpha, float nox)	// the square root term shared by G1 and G2
{
	return sqrt(alpha * alpha + (1.0 - alpha * alpha) * nox * nox);
}

// the specular lobe's value times the cosine at the light: f * (n.wi)
vec3 SpecularTimesCos(Surface s, vec3 wi)
{
	const float nol = dot(s.n, wi), nov = dot(s.n, s.wo);
	if (nol <= 0.0 || nov <= 0.0)
		return vec3(0.0);
	const vec3 h = normalize(s.wo + wi);
	const float noh = dot(s.n, h), voh = dot(s.wo, h);
	const float a2 = s.alpha * s.alpha;
	const float d = noh * noh * (a2 - 1.0) + 1.0;
	const float ndf = a2 / (PI * d * d);
	const float vis = 0.5 / (nol * SmithLambda(s.alpha, nov) + nov * SmithLambda(s.alpha, nol));
	return Fresnel(s.f0, voh) * (ndf * vis * nol);
}

// Light arriving from direction wi with radiance-times-solid-angle e
// (irradiance on a surface facing it), through each lobe
Lit Reflect(Surface s, vec3 wi, vec3 e)
{
	Lit lit = Lit(vec3(0.0), vec3(0.0));
	if (s.medium)
	{
		lit.diffuse = e;		// air has no facing
		return lit;
	}
	const float nol = dot(s.n, wi);
	if (nol <= 0.0)
		return lit;
	lit.diffuse = e * nol;
	if (s.light_sampled_spec)
		lit.specular = e * SpecularTimesCos(s, wi);
	return lit;
}

float Importance(Surface s, Lit l)
{
	return Luminance(s.kd * l.diffuse) * INV_PI + Luminance(l.specular);
}

vec3 ClampSample(vec3 c, float max_luminance)
{
	const float lum = Luminance(c);
	if (isnan(lum) || isinf(lum))
		return vec3(0.0);
	return lum > max_luminance ? c * (max_luminance / lum) : c;
}

// the point a ray leaves a surface from; air has nowhere to stand off
vec3 Leave(Surface s)
{
	return s.medium ? s.p : s.p + s.ng * RAY_OFFSET;
}

// ----------------------------------------------------------------- lights

int GridCell(vec3 p)
{
	const ivec3 c = clamp(ivec3((p - fr.grid_origin.xyz) * fr.grid_origin.w), ivec3(0), fr.grid_dims.xyz - 1);
	return (c.z * fr.grid_dims.y + c.y) * fr.grid_dims.x + c.x;
}

// Light thrown back up by a simulated liquid surface, onto walls and
// ceilings near water. The surface is treated as a level mirror for the
// light just sampled, so its image lies as far below the surface as the
// light is above. y is the point on the light, e what it would send
// straight here.
void WaterBounce(Surface s, vec3 y, vec3 e, inout Lit lit)
{
	if (s.medium)
		return;
	// The body the light would come off: of those whose surface the path
	// crosses, the one nearest below. Only that one is tried, so that a map
	// with many bodies costs no more rays than one with few.
	int body = -1;
	float z = -1.0e30;
	vec3 q = vec3(0.0);		// where the path meets the surface
	for (int i = 0; i < fr.size.z; i++)
	{
		const float level = fr.water_at[i].x;
		if (fr.water_at[i].y < 0.0 || s.p.z <= level + 1.0 || y.z <= level + 1.0 || s.p.z - level > 512.0 || level <= z)
			continue;
		const vec3 d = vec3(y.xy, 2.0 * level - y.z) - s.p;
		const vec3 at = s.p + d * ((level - s.p.z) / d.z);
		const vec4 rect = fr.water_rect[i];
		if (at.x < rect.x || at.x > rect.z || at.y < rect.y || at.y > rect.w)
			continue;
		// the caustic picture also says where there is liquid at all
		const int caustics = int(fr.water_at[i].w);
		if (caustics >= 0 && Texel(caustics, (at.xy - fr.water_wave[i].xy) * fr.water_wave[i].zw, false).a < 0.5)
			continue;
		body = i;
		z = level;
		q = at;
	}
	if (body < 0)
		return;

	const vec3 d = vec3(y.xy, 2.0 * z - y.z) - s.p;
	const float len2 = dot(d, d);
	const vec3 wi = d * inversesqrt(len2);
	const float m = 1.0 + wi.z;	// 1 - cosine of the angle at the water
	const float fresnel = 0.02 + 0.98 * m * m * m * m * m;

	// e was for the straight path; this one is as long as the way to the image
	const vec3 straight = y - s.p;
	const Lit add = Reflect(s, wi, e * (fresnel * dot(straight, straight) / len2));
	if (Importance(s, add) <= 0.0)
		return;

	// both legs must be clear
	const vec3 above = q + vec3(0.0, 0.0, 0.1);
	if (Visible(Leave(s), above) <= 0.0 || Visible(above, y) <= 0.0)
		return;

	lit.diffuse += add.diffuse;
	lit.specular += add.specular;
}

// The map's lights. A handful of candidates are weighed by the light they
// would put on the surface, which is cheap; one is drawn in proportion, and
// only for that one is it found out whether anything is in the way.
Lit DirectLights(Surface s, bool first_hit)
{
	Lit none = Lit(vec3(0.0), vec3(0.0));
	const int num_lights = fr.counts.x;
	if (num_lights <= 0)
		return none;
	const int candidates = max(1, first_hit ? fr.settings.y : fr.settings.y / 2);

	float wsum = 0.0;
	vec3 chosen_y = vec3(0.0), chosen_wi = vec3(0.0), chosen_e = vec3(0.0);
	float chosen_phat = 0.0;

	const bool has_grid = fr.grid_dims.w != 0;
	const int cell = has_grid ? GridCell(s.p) : 0;
	const int in_cell = has_grid ? int(indices.v[fr.table_at2.w + cell]) : 0;
	const float use_global = in_cell > 0 ? GLOBAL_LIGHT_CHANCE : 1.0;
	const int cell_lights = fr.table_at2.z + cell * PER_CELL;
	const int cell_pdfs = fr.table_at.y + cell * PER_CELL;
	const int cell_keep = fr.table_at.z + cell * PER_CELL;
	const int cell_sorted = fr.table_at3.y + cell * PER_CELL;

	// Mostly from the list for this cell, sometimes from the whole map so
	// nothing is missed: use_global of the candidates on average. Which ones
	// goes by turn, the first so many, and by chance only for the one that
	// stands for what is left over of that number. Drawn by chance each,
	// pixels traced side by side took different ways through here at every
	// candidate, and the card, which runs them in step, had all of them wait
	// out the longer way, the search of the cell's list below, nearly every
	// time. Each light still has the chance it had of being a candidate, taken
	// over them all, which is what the weights go by.
	const float want_global = use_global * float(candidates);
	const int sure_global = int(want_global);
	const float last_global = want_global - float(sure_global);

	for (int i = 0; i < candidates; i++)
	{
		// Either way with no search: a place is drawn evenly, and the light
		// there is kept as often as the tables say, or else the one they
		// name is taken in its stead, which gives each light the chance it
		// should have (see PickingTable in pt_rtx.cpp).
		uint li;
		float cell_pdf = 0.0;
		if (i < sure_global || (i == sure_global && Rand() < last_global))
		{
			const int at = min(int(Rand() * float(num_lights)), num_lights - 1);
			li = Rand() < tables.v[fr.table_at.x + at] ? uint(at) : indices.v[fr.table_at3.x + at];
			// it may be in the cell's list as well, with a chance of being picked from there
			const int j = PlaceInCell(cell_sorted, in_cell, li);
			if (j >= 0)
				cell_pdf = tables.v[cell_pdfs + j];
		}
		else
		{
			// (a light of the list, and in its low five bits the place of its stand-in)
			const int at = min(int(Rand() * float(in_cell)), in_cell - 1);
			const int j = Rand() < tables.v[cell_keep + at] ? at : int(indices.v[cell_lights + at] & 31u);
			li = indices.v[cell_lights + j] >> 5;
			cell_pdf = tables.v[cell_pdfs + j];
		}
		const Light l = world_lights.l[li];
		float pdf = use_global * l.pdf + (1.0 - use_global) * cell_pdf;

		vec3 y, e, light_n = vec3(0.0);
		const bool area = l.tri < POINT_LIGHT;
		if (area)
		{
			const vec3 p0 = l.origin;
			const vec3 e1 = l.dir, e2 = l.edge;
			float a = Rand(), b = Rand();
			if (a + b > 1.0)
			{
				a = 1.0 - a;
				b = 1.0 - b;
			}
			y = p0 + e1 * a + e2 * b;
			e = l.emission;
			const vec3 x = cross(e1, e2);
			const float len = length(x);
			if (len <= 0.0)
				continue;
			light_n = x / len;
			pdf /= len * 0.5;
		}
		else
		{
			y = l.origin;
			const int style = int(l.tri - POINT_LIGHT);
			const float scale = style > 0 ? tables.v[style] : 1.0;
			if (scale <= 0.0)
				continue;
			e = l.emission * scale;
		}

		const vec3 d = y - s.p;
		const float dist2 = dot(d, d);
		if (dist2 <= 1.0e-6)
			continue;
		const vec3 wi = d * inversesqrt(dist2);
		if (l.cone_cos > 0.0 && !area && -dot(wi, l.dir) < l.cone_cos)
			continue;		// outside the spotlight's cone
		const float nol = s.medium ? 1.0 : dot(s.n, wi);
		if (nol <= 0.0)
			continue;
		float geom = 1.0 / dist2;
		if (area)
		{
			const float cosy = -dot(light_n, wi);
			if (cosy <= 0.0)
				continue;
			geom *= cosy;
		}
		e *= geom;

		const float phat = Luminance(e) * nol;
		if (phat <= 0.0 || pdf <= 0.0)
			continue;
		const float weight = phat / pdf;
		wsum += weight;
		if (Rand() * wsum < weight)
		{
			chosen_y = y;
			chosen_wi = wi;
			chosen_e = e;
			chosen_phat = phat;
		}
	}

	if (wsum <= 0.0)
		return none;
	chosen_e *= wsum / (float(candidates) * chosen_phat);

	// Light bends going into a simulated liquid, so a shadow under one
	// falls where the bent light is cut off. For what the eye sees itself:
	// light that has bounced takes the liquid for no more than it looks.
	// (What its waves do to the light is laid over what the eye sees: see
	// AddWater.)
	vec3 crossing = vec3(0.0);
	float gets_in = 1.0;
	bool bent = false;
	if (first_hit && fr.size.z > 0)
		bent = Bend(Leave(s), chosen_y, crossing, gets_in);

	Lit lit = none;
	const float clear = Visible(Leave(s), chosen_y, bent, crossing) * gets_in;
	if (clear > 0.0)
		lit = Reflect(s, chosen_wi, chosen_e * clear);
	if (fr.size.z > 0)
		WaterBounce(s, chosen_y, chosen_e, lit);
	return lit;
}

// a direction drawn where the sky is bright, in the sky's own frame, and the
// chance per unit solid angle of drawing it
vec3 SampleSky(out float pdf)
{
	const int res = fr.table_at2.y;
	const int count = 6 * res * res;
	const int pick = min(UpperBound(fr.table_at2.x, count, Rand()), count - 1);
	const int face = pick / (res * res);
	const int texel = pick % (res * res);
	const float u = (float(texel % res) + Rand()) / float(res), v = (float(texel / res) + Rand()) / float(res);

	const int a = face / 2;
	vec3 d;
	d[a] = (face & 1) != 0 ? -1.0 : 1.0;
	d[(a + 1) % 3] = 2.0 * u - 1.0;
	d[(a + 2) % 3] = 2.0 * v - 1.0;
	d = normalize(d);

	// texels were weighted by luminance times solid angle, so per unit solid
	// angle the chance is just the luminance over its integral
	pdf = Luminance(SkyAt(d)) / fr.sky_misc.z;
	return d;
}

// Light from the sky: a direction is drawn where the sky is bright and the
// light counts if nothing solid is in the way. Only tried as often as the
// sky can be seen from around here at all.
Lit DirectSky(Surface s)
{
	Lit none = Lit(vec3(0.0), vec3(0.0));
	if (fr.table_at2.y <= 0 || fr.sky_misc.z <= 0.0)
		return none;

	float chance = 1.0;
	if (fr.grid_dims.w != 0)
		chance = tables.v[fr.table_at.w + GridCell(s.p)];
	if (chance <= 0.0 || Rand() >= chance)
		return none;

	float pdf;
	const vec3 sky_dir = SampleSky(pdf);
	if (pdf <= 0.0)
		return none;
	const vec3 wi = Turn(sky_dir, fr.sky_turn.w);
	if (!s.medium && (dot(s.n, wi) <= 0.0 || dot(s.ng, wi) <= 0.0))
		return none;

	Hit hit;
	Material mat;
	float tmin = 0.0;
	if (!Closest(Leave(s), wi, tmin, false, true, hit, mat) || (mat.flags & MAT_SKY) == 0u)
		return none;

	return Reflect(s, wi, SkyAt(sky_dir) * (1.0 / (pdf * chance)));
}

Lit DirectWorld(Surface s, bool first_hit)
{
	if (fr.bases.x == VIEW_FURNACE)
		return Lit(vec3(0.0), vec3(0.0));		// no light is lit
	Lit lit = DirectLights(s, first_hit);
	const Lit sky = DirectSky(s);
	lit.diffuse += sky.diffuse;
	lit.specular += sky.specular;
	return lit;
}

// a point light's contribution, ignoring occlusion
Lit PointLight(Surface s, Light l)
{
	const vec3 d = l.origin - s.p;
	const float dist2 = dot(d, d);
	if (dist2 <= 1.0e-6)
		return Lit(vec3(0.0), vec3(0.0));
	return Reflect(s, d * inversesqrt(dist2), l.emission / dist2);
}

// A point on a ball of light, drawn evenly over as much of the ball as shows
// from p: y is the point and wi the way to it. Returns what it sends to p
// over the chance of its being drawn, which comes to the same wherever on
// the ball it is: the ball's radiance times the solid angle the ball fills.
// From inside it, nothing.
vec3 SampleBall(Light l, vec3 p, out vec3 y, out vec3 wi)
{
	const vec3 d = l.origin - p;
	const float dist2 = dot(d, d), r2 = l.radius * l.radius;
	y = l.origin;
	wi = vec3(0.0, 0.0, 1.0);
	if (dist2 <= r2)
		return vec3(0.0);
	const float dist = sqrt(dist2);
	const float sin2_max = r2 / dist2;
	// 1 - cos of the angle from its middle to its edge, which far off is too small to take from 1
	const float cap = sin2_max / (1.0 + sqrt(1.0 - sin2_max));

	const float cos_t = 1.0 - Rand() * cap, phi = 2.0 * PI * Rand();
	const float sin2_t = max(0.0, 1.0 - cos_t * cos_t), sin_t = sqrt(sin2_t);
	vec3 t, b;
	const vec3 w = d / dist;
	Basis(w, t, b);
	wi = t * (sin_t * cos(phi)) + b * (sin_t * sin(phi)) + w * cos_t;
	// where going that way meets the ball
	y = p + wi * (dist * cos_t - sqrt(max(0.0, r2 - dist2 * sin2_t)));
	return l.emission * (2.0 * cap / r2);		// intensity / (pi r^2) * 2 pi cap
}

// One of the frame's lights on a surface, with nothing in the way; at is
// where to look to see whether anything is. A point gives its all, a ball
// what one point drawn on it stands for.
Lit FrameLight(Surface s, Light l, out vec3 at)
{
	if (l.radius <= 0.0)
	{
		at = l.origin;
		return PointLight(s, l);
	}
	vec3 wi;
	const vec3 e = SampleBall(l, s.p, at, wi);
	return Reflect(s, wi, e);
}

// How much one of the frame's lights is worth to a surface beside the
// others. For a point that is what it gives; a ball is weighed as if it were
// all at its middle, but for where the middle is under the surface's horizon
// and some of the ball over it: there it is not to be left out.
float FrameLightWeight(Surface s, Light l)
{
	if (l.radius <= 0.0 || s.medium)
		return Importance(s, PointLight(s, l));
	const vec3 d = l.origin - s.p;
	const float dist2 = dot(d, d);
	if (dist2 <= l.radius * l.radius)
		return 0.0;
	const float dist = sqrt(dist2);
	const vec3 wi = d / dist, e = l.emission / dist2;
	const float rise = l.radius / dist, nol = dot(s.n, wi);
	if (nol <= -rise)
		return 0.0;
	float weight = Luminance((s.kd * INV_PI + s.f0 * 0.05) * e) * max(nol, 0.25 * (nol + rise));
	if (s.light_sampled_spec)
		weight += Luminance(e * SpecularTimesCos(s, wi));
	return weight;
}

// The frame's lights (muzzle flashes, explosions), or of them only its balls
// of light: one, picked in proportion to what it would give with nothing in
// the way, which for a point is known exactly.
Lit FrameOne(Surface s, bool balls)
{
	Lit none = Lit(vec3(0.0), vec3(0.0));
	const int count = fr.counts.y;
	if (count <= 0 || fr.bases.x == VIEW_FURNACE)
		return none;

	float total = 0.0;
	for (int i = 0; i < count; i++)
		if (!balls || frame_lights.l[i].radius > 0.0)
			total += FrameLightWeight(s, frame_lights.l[i]);
	if (total <= 0.0)
		return none;

	float pick = Rand() * total;
	for (int i = 0; i < count; i++)
	{
		if (balls && frame_lights.l[i].radius <= 0.0)
			continue;
		const float imp = FrameLightWeight(s, frame_lights.l[i]);
		pick -= imp;
		if (pick <= 0.0 && imp > 0.0)
		{
			vec3 at;
			Lit f = FrameLight(s, frame_lights.l[i], at);
			if (Importance(s, f) <= 0.0)
				return none;
			const float clear = Visible(Leave(s), at);
			if (clear <= 0.0)
				return none;
			f.diffuse *= clear * total / imp;
			f.specular *= clear * total / imp;
			return f;
		}
	}
	return none;
}

Lit DirectFrameOne(Surface s)
{
	return FrameOne(s, false);
}

// one of its balls of light: a point is drawn on the ball, so what comes of
// it is as noisy as the map's lights are and belongs with them
Lit DirectFrameBall(Surface s)
{
	return FrameOne(s, true);
}

// all of its point lights, exactly: for what the eye sees directly
Lit DirectFrameAll(Surface s)
{
	Lit sum = Lit(vec3(0.0), vec3(0.0));
	if (fr.bases.x == VIEW_FURNACE)
		return sum;
	for (int i = 0; i < fr.counts.y; i++)
	{
		if (frame_lights.l[i].radius > 0.0)
			continue;
		const Lit f = PointLight(s, frame_lights.l[i]);
		if (Importance(s, f) > 0.0 && Visible(Leave(s), frame_lights.l[i].origin) > 0.0)
		{
			sum.diffuse += f.diffuse;
			sum.specular += f.specular;
		}
	}
	return sum;
}

// What every light, the map's and the frame's, would put on the surface
// were nothing in the way, as the irradiance on the matte part and the
// radiance off the shine: for export, see img_export. The map's triangle
// lights are taken as points at their middle, facing as they do, and the
// frame's balls as points. The sky is not counted. A loop over every light
// for every pixel: not for play.
void DirectEveryLight(Surface s, out vec3 diffuse, out vec3 specular)
{
	diffuse = vec3(0.0);
	specular = vec3(0.0);
	const int of_map = fr.counts.x;
	for (int i = 0; i < of_map + fr.counts.y; i++)
	{
		const Light l = i < of_map ? world_lights.l[i] : frame_lights.l[i - of_map];
		vec3 y = l.origin, e = l.emission, light_n = vec3(0.0);
		const bool area = i < of_map && l.tri < POINT_LIGHT;
		if (area)
		{
			y += (l.dir + l.edge) * (1.0 / 3.0);
			const vec3 x = cross(l.dir, l.edge);
			const float len = length(x);
			if (len <= 0.0)
				continue;
			light_n = x / len;
			e *= len * 0.5;
		}
		else if (i < of_map)
		{
			const int style = int(l.tri - POINT_LIGHT);
			e *= style > 0 ? tables.v[style] : 1.0;
		}
		const vec3 d = y - s.p;
		const float dist2 = dot(d, d);
		if (dist2 <= 1.0e-6)
			continue;
		const vec3 wi = d * inversesqrt(dist2);
		if (l.cone_cos > 0.0 && !area && -dot(wi, l.dir) < l.cone_cos)
			continue;
		const float nol = dot(s.n, wi);
		if (nol <= 0.0)
			continue;
		float geom = 1.0 / dist2;
		if (area)
		{
			const float cosy = -dot(light_n, wi);
			if (cosy <= 0.0)
				continue;
			geom *= cosy;
		}
		e *= geom;
		diffuse += e * nol;
		specular += e * SpecularTimesCos(s, wi);
	}
}

// How much the frame's lights, which come and go, put on the pixel: what
// the point lights put on the surface, which was lit exactly, what the
// balls would put on it with nothing in the way, and what all of them light
// the air in front of it by with nothing in the way, along the view ray to
// reach. There is no noise in it, so from frame to frame it says where light
// has changed, and by how much of the whole, which the gathered light
// cannot say of itself.
float FrameLightLevel(Surface s, vec3 flash_light, vec3 eye, vec3 dir, float reach, float fog)
{
	float level = Luminance(flash_light);
	for (int i = 0; i < fr.counts.y; i++)
	{
		const Light l = frame_lights.l[i];
		if (l.radius > 0.0)
			level += Importance(s, PointLight(s, l));
		if (fog > 0.0)
		{
			const vec3 d = l.origin - eye;
			const float t0 = dot(d, dir);
			const float h = max(sqrt(max(dot(d, d) - t0 * t0, 0.0)), 1.0);
			level += Luminance(l.emission) * fog * (0.25 * INV_PI) * (atan((reach - t0) / h) + atan(t0 / h)) / h;
		}
	}
	return level;
}

// light on a point in the air, from all round
vec3 DirectMedium(vec3 p)
{
	Surface s;
	s.medium = true;
	s.froth = 0.0;
	s.p = p;
	s.ng = vec3(0.0, 0.0, 1.0);
	s.n = s.ng;
	s.wo = s.ng;
	s.kd = vec3(1.0);
	s.f0 = vec3(0.0);
	s.roughness = 1.0;
	s.metallic = 0.0;
	s.alpha = 1.0;
	s.light_sampled_spec = false;
	return DirectWorld(s, false).diffuse + DirectFrameOne(s).diffuse;
}

// ------------------------------------------------------------------ paths

// keeps a sampled direction on the outside of the real surface
vec3 AboveSurface(Surface s, vec3 wi)
{
	const float below = dot(wi, s.ng);
	return below < 0.0 ? wi - s.ng * (2.0 * below) : wi;
}

vec3 SampleDiffuse(Surface s)
{
	const float r1 = Rand(), r2 = Rand();
	const float r = sqrt(r1), phi = 2.0 * PI * r2;
	vec3 t, b;
	Basis(s.n, t, b);
	return AboveSurface(s, t * (r * cos(phi)) + b * (r * sin(phi)) + s.n * sqrt(max(0.0, 1.0 - r1)));
}

// Samples the normals visible from wo (Heitz, "Sampling the GGX Distribution
// of Visible Normals", JCGT 2018) and reflects wo about the one drawn
bool SampleSpecular(Surface s, out vec3 wi, out vec3 weight)
{
	vec3 t, b;
	Basis(s.n, t, b);
	const vec3 ve = vec3(dot(s.wo, t), dot(s.wo, b), dot(s.wo, s.n));
	wi = vec3(0.0);
	weight = vec3(0.0);
	if (ve.z <= 0.0)
		return false;

	const float a = s.alpha;
	const vec3 vh = normalize(vec3(a * ve.x, a * ve.y, ve.z));
	const float lensq = vh.x * vh.x + vh.y * vh.y;
	const vec3 t1 = lensq > 0.0 ? vec3(-vh.y, vh.x, 0.0) * inversesqrt(lensq) : vec3(1.0, 0.0, 0.0);
	const vec3 t2 = cross(vh, t1);

	const float u1 = Rand(), u2 = Rand();
	const float r = sqrt(u1), phi = 2.0 * PI * u2;
	const float p1 = r * cos(phi);
	float p2 = r * sin(phi);
	const float sblend = 0.5 * (1.0 + vh.z);
	p2 = (1.0 - sblend) * sqrt(max(0.0, 1.0 - p1 * p1)) + sblend * p2;
	const vec3 nh = t1 * p1 + t2 * p2 + vh * sqrt(max(0.0, 1.0 - p1 * p1 - p2 * p2));
	const vec3 hl = normalize(vec3(a * nh.x, a * nh.y, max(0.0, nh.z)));
	const vec3 h = t * hl.x + b * hl.y + s.n * hl.z;

	const float voh = dot(s.wo, h);
	wi = h * (2.0 * voh) - s.wo;
	const float nol = dot(s.n, wi), nov = ve.z;
	if (nol <= 0.0 || voh <= 0.0)
		return false;
	wi = AboveSurface(s, wi);

	// value * cos / pdf comes down to F * G2 / G1(wo)
	const float lv = SmithLambda(a, nov), ll = SmithLambda(a, nol);
	const float g2_over_g1 = nol * (nov + lv) / (nov * ll + nol * lv);
	weight = Fresnel(s.f0, voh) * g2_over_g1;
	return true;
}

// A path of light followed back from surface to surface, a step at a time:
// PathMeet takes it to the next surface, the light falling there is found,
// and PathGoOn adds what that surface sends back and picks the way on.
// Radiance does the whole of one; a pixel with several to follow takes a step
// of whichever it is on at each turn of one loop (see trace.comp), which is
// why they come apart like this.
struct Path
{
	vec3	origin, dir;		// the ray to follow next
	vec3	radiance;			// the light brought back so far
	vec3	throughput;			// how much of what is found from here on gets back
	bool	count_emitters;		// lights it runs into count, because nothing has sampled them for it
	int		depth, max_bounces;
	float	reached;			// how far off the first thing it met was
	bool	first;
	bool	ended;
};

// rays_followed is raised by the number of rays a path was made of
int rays_followed = 0;

Path PathFrom(vec3 origin, vec3 dir, bool count_emitters, int depth, int max_bounces)
{
	Path p;
	p.origin = origin;
	p.dir = dir;
	p.radiance = vec3(0.0);
	p.throughput = vec3(1.0);
	p.count_emitters = count_emitters;
	p.depth = depth;
	p.max_bounces = max_bounces;
	p.reached = 1.0e30;
	p.first = true;
	p.ended = false;
	return p;
}

// Follows the path's ray to the surface it meets, s. False if the path ends
// there with nothing to light: it met nothing, or the sky, or something that
// reflects nothing. shiny: the surface has a shine to it.
bool PathMeet(inout Path p, out Surface s, out bool shiny)
{
	// in the white furnace every path ends in the same light
	const bool furnace = fr.bases.x == VIEW_FURNACE;
	shiny = false;

	Hit hit;
	Material base;
	float tmin = 0.0;
	rays_followed++;
	if (!Closest(p.origin, p.dir, tmin, false, true, hit, base))
	{
		if (furnace)
			p.radiance += p.throughput * FURNACE_LIGHT;
		p.ended = true;
		return false;
	}
	if (p.first)
	{
		p.reached = hit.t;
		p.first = false;
	}
	if ((base.flags & MAT_SKY) != 0u)
	{
		if (furnace)
			p.radiance += p.throughput * FURNACE_LIGHT;
		else if (p.count_emitters || fr.table_at2.y <= 0)
			p.radiance += p.throughput * Sky(p.dir);
		p.ended = true;
		return false;
	}

	MakeSurface(hit, base, p.origin, p.dir, false, s);

	if ((s.mat.bits & BIT_EMISSIVE) != 0u && s.front && (p.count_emitters || (s.mat.bits & BIT_SAMPLED) == 0u))
		p.radiance += p.throughput * Emitted(s, false);

	const float ld = Luminance(s.kd), ls = Luminance(SpecularAlbedo(s));
	if (ld + ls <= 0.0)
	{
		p.ended = true;		// reflects nothing
		return false;
	}
	shiny = ls > 0.0;
	return true;
}

// What the surface the path has met sends back along it, given the light
// that falls there, and the way the path goes on from it, if it does.
// mirrored and shine are as SampleSpecular drew them for the surface, shine
// nothing if it drew none: the shine takes its share of the light first and
// the matte part has what is left, so the two together never reflect more
// than falls on them. The share is that of one way the light could be
// mirrored, which is followed if the path goes that way.
void PathGoOn(inout Path p, Surface s, vec3 mirrored, vec3 shine, Lit world, Lit frame)
{
	const bool furnace = fr.bases.x == VIEW_FURNACE;
	const vec3 matte = s.kd * (1.0 - shine);

	p.radiance += p.throughput * (matte * (world.diffuse + frame.diffuse) * INV_PI + world.specular + frame.specular);

	if (p.depth >= p.max_bounces)
	{
		if (furnace)
			p.radiance += p.throughput * FURNACE_LIGHT;
		p.ended = true;
		return;
	}

	vec3 wi;
	if (fr.settings.z < 2)
	{
		// Shiny surfaces still show highlights from lights, but nothing
		// is followed off them: the matte part stands in for the shine,
		// and carries its share too.
		if (Luminance(s.kd) <= 0.0)
		{
			p.ended = true;
			return;
		}
		wi = SampleDiffuse(s);
		p.throughput *= s.kd;
		p.count_emitters = false;
	}
	else
	{
		// continue through one lobe, chosen by how much each reflects
		const float lm = Luminance(matte), lh = Luminance(shine);
		if (lm + lh <= 0.0)
		{
			p.ended = true;
			return;
		}
		const float pick_spec = lh / (lm + lh);
		if (Rand() < pick_spec)
		{
			wi = mirrored;
			p.throughput *= shine * (1.0 / pick_spec);
			p.count_emitters = !s.light_sampled_spec;
		}
		else
		{
			wi = SampleDiffuse(s);
			p.throughput *= matte * (1.0 / (1.0 - pick_spec));
			p.count_emitters = false;
		}
	}

	// paths that carry little are ended at random, the rest made to count for them
	if (p.depth >= 1)
	{
		const float survive = clamp(MaxComponent(p.throughput), 0.1, 1.0);
		if (Rand() >= survive)
		{
			p.ended = true;
			return;
		}
		p.throughput *= 1.0 / survive;
	}

	p.origin = s.p + s.ng * RAY_OFFSET;
	p.dir = wi;
	p.depth++;
}

// The light a ray brings back, following it from surface to surface.
// count_emitters: lights it runs into count, because nothing has sampled
// them for it. reached: how far off the first thing it met was.
vec3 Radiance(vec3 origin, vec3 dir, bool count_emitters, int depth, int max_bounces, out float reached)
{
	Path p = PathFrom(origin, dir, count_emitters, depth, max_bounces);
	while (!p.ended)
	{
		Surface s;
		bool shiny;
		if (!PathMeet(p, s, shiny))
			break;
		vec3 mirrored, shine;
		if (!shiny || !SampleSpecular(s, mirrored, shine))
			shine = vec3(0.0);
		const Lit world = DirectWorld(s, false), frame = DirectFrameOne(s);
		PathGoOn(p, s, mirrored, shine, world, frame);
	}
	reached = p.reached;
	return p.radiance;
}

#endif
