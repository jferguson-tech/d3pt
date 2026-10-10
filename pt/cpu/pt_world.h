// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Jonathan Ferguson
//
// What the tracer traces: the static world and the per frame scene.
#pragma once

#include "../include/pt.h"
#include "pt_bvh.h"

#include <memory>
#include <vector>

namespace PT_NS {

const uint32_t kDynamic = 0x80000000u;	// triangle index bit: belongs to the frame, not the world

void InitColourTables();
extern float g_to_linear[256];

inline Vec3 Decode(uint32_t rgba)
{
	return Vec3(g_to_linear[rgba & 0xff], g_to_linear[(rgba >> 8) & 0xff], g_to_linear[(rgba >> 16) & 0xff]);
}

struct Texture
{
	int						width = 0, height = 0;
	std::vector<uint32_t>	pixels;
	Vec3					average{1, 1, 1};

	void Set(const pt_texture_t &src);

	uint32_t Texel(float u, float v) const
	{
		int x = (int)std::floor(u * width) % width;
		int y = (int)std::floor(v * height) % height;
		if (x < 0) x += width;
		if (y < 0) y += height;
		return pixels[(size_t)y * width + x];
	}

	// the four texels around a point and how much each counts, wrapping
	void Corners(float u, float v, uint32_t texel[4], float weight[4]) const
	{
		const float fx = u * width - 0.5f, fy = v * height - 0.5f;
		const float flx = std::floor(fx), fly = std::floor(fy);
		const float ax = fx - flx, ay = fy - fly;
		int x0 = (int)flx % width, y0 = (int)fly % height;
		if (x0 < 0) x0 += width;
		if (y0 < 0) y0 += height;
		const int x1 = x0 + 1 == width ? 0 : x0 + 1, y1 = y0 + 1 == height ? 0 : y0 + 1;
		texel[0] = pixels[(size_t)y0 * width + x0];
		texel[1] = pixels[(size_t)y0 * width + x1];
		texel[2] = pixels[(size_t)y1 * width + x0];
		texel[3] = pixels[(size_t)y1 * width + x1];
		weight[0] = (1.0f - ax) * (1.0f - ay);
		weight[1] = ax * (1.0f - ay);
		weight[2] = (1.0f - ax) * ay;
		weight[3] = ax * ay;
	}

	// bilinear, in linear light
	Vec3 Smooth(float u, float v) const
	{
		uint32_t t[4];
		float w[4];
		Corners(u, v, t, w);
		return Decode(t[0]) * w[0] + Decode(t[1]) * w[1] + Decode(t[2]) * w[2] + Decode(t[3]) * w[3];
	}

	// clamped, for the sky faces
	Vec3 SampleClamped(float u, float v) const
	{
		int x = (int)(u * width);
		int y = (int)(v * height);
		x = x < 0 ? 0 : (x >= width ? width - 1 : x);
		y = y < 0 ? 0 : (y >= height ? height - 1 : y);
		return Decode(pixels[(size_t)y * width + x]);
	}
};

struct Material
{
	const Texture	*texture = nullptr;
	const Texture	*normal_texture = nullptr;
	const Texture	*emission_map = nullptr;	// what glows, in place of the rules below
	const Material	*anim_next = nullptr;
	int				anim_length = 1;	// materials in the animation this one starts
	Vec3			emission;			// average
	Vec3			emission_per_texel;	// multiply by the texel to get emitted radiance
	float			alpha = 1.0f;
	float			emission_seen = 0.0f;
	float			scroll_u = 0.0f, scroll_v = 0.0f;
	int				wave_map = 0, caustic_map = 0, foam_map = 0;	// handle + 1
	float			wave_rect[4] = {0, 0, 0, 0};
	Vec3			absorb;
	float			roughness = 1.0f;
	float			metallic = 0.0f;
	uint32_t		flags = 0;
	bool			emissive = false;
	bool			sampled = false;	// reached through the light lists, so not counted when hit by chance

	void Set(const pt_material_t &src, const Texture *tex, const Texture *normal_tex, const Texture *emission_tex);

	// the material showing at this step of its animation
	const Material &At(int frame) const
	{
		const Material *m = this;
		for (int i = anim_length > 1 ? frame % anim_length : 0; i > 0 && m->anim_next; i--)
			m = m->anim_next;
		return *m;
	}
};

struct Tri
{
	Vec3			p0, e1, e2;
	Vec3			n;			// unit, towards the counter clockwise side
	Vec3			tu, tv;		// unit directions in which u and v grow
	Vec3			vn[3];		// vertex normals, when smooth
	float			area;
	float			uv[3][2];
	const Material	*mat;
	bool			smooth;

	void Set(Vec3 a, Vec3 b, Vec3 c);	// needs uv filled in
};

struct Light
{
	uint32_t	tri;		// or ~0u for a point light
	Vec3		origin;		// point lights; centroid for triangles
	Vec3		emission;	// radiance for triangles, intensity for points
	float		pdf;		// chance of being picked map wide
	int			style;		// point lights: which light style scales it
	Vec3		dir;		// spotlights: where it points
	float		cone_cos;	// and how wide; 0 = all round
	float		radius = 0.0f;	// the frame's: above 0 a ball of light, not a point
};

// For each cell of a coarse grid, the lights that matter most there. Sampling
// from the cell a point is in finds nearby lights far more often than picking
// by power over the whole map.
struct LightGrid
{
	static const int kPerCell = 24;

	Vec3		origin;
	float		inv_cell = 0.0f;
	int			dims[3] = {0, 0, 0};
	std::vector<uint32_t>	light;	// kPerCell per cell
	std::vector<float>		pdf;	// chance within the cell
	std::vector<float>		cdf;
	std::vector<uint8_t>	count;

	size_t Cell(Vec3 p) const
	{
		size_t c[3];
		for (int a = 0; a < 3; a++)
		{
			int i = (int)((p[a] - origin[a]) * inv_cell);
			c[a] = (size_t)(i < 0 ? 0 : (i >= dims[a] ? dims[a] - 1 : i));
		}
		return (c[2] * dims[1] + c[1]) * dims[0] + c[0];
	}
};

struct World
{
	std::vector<Texture>	textures;
	std::vector<Material>	materials;
	std::vector<Tri>		tris;
	Bvh						bvh;
	std::vector<Light>		lights;
	std::vector<float>		light_cdf;
	LightGrid				grid;
	int						sky[6] = {-1, -1, -1, -1, -1, -1};
	float					sky_scale = 1.0f;
	bool					has_waves = false;

	// a simulated body of liquid: where its surface is, a material that
	// carries its maps, whether any of it is seen from above, and where the
	// light on it comes from: the middle of the lights that shine on it,
	// over each of kLamps by kLamps parts of its extent, row by row from
	// min_x, min_y
	struct Water
	{
		static const int kLamps = 4;

		float			min_x, min_y, max_x, max_y, z;
		const Material	*mat;
		bool			top;
		Vec3			lamps[kLamps * kLamps];

		// where the light comes from over a point of it
		Vec3 Lamp(float x, float y) const
		{
			const float fx = std::min(std::max((x - min_x) / (max_x - min_x) * (float)kLamps - 0.5f, 0.0f), (float)(kLamps - 1));
			const float fy = std::min(std::max((y - min_y) / (max_y - min_y) * (float)kLamps - 0.5f, 0.0f), (float)(kLamps - 1));
			const int x0 = std::min((int)fx, kLamps - 2), y0 = std::min((int)fy, kLamps - 2);
			const float ax = fx - (float)x0, ay = fy - (float)y0;
			const Vec3 *row = &lamps[y0 * kLamps + x0];
			return (row[0] * (1.0f - ax) + row[1] * ax) * (1.0f - ay) + (row[kLamps] * (1.0f - ax) + row[kLamps + 1] * ax) * ay;
		}
	};
	std::vector<Water>		waters;

	Vec3 Sky(Vec3 d) const;

	// The sky as a light: a direction drawn in proportion to how bright the
	// sky is there, and the chance per unit solid angle of drawing it. Both
	// are in the sky's own frame, before any turning.
	std::vector<float>		sky_cdf;		// over 6 faces of sky_res * sky_res texels
	int						sky_res = 0;
	float					sky_total = 0.0f;	// integral of luminance over the sphere
	Vec3 SampleSky(Rng &rng, float &pdf) const;

	// per light grid cell, how often it is worth looking for the sky from there
	std::vector<float>		sky_chance;
};

// what the host hands over each frame
struct Frame
{
	std::vector<Material>	materials;
	std::vector<Tri>		tris;
	std::vector<Vec3>		prev;		// 3 per triangle: its corners last frame; may be empty
	Bvh						bvh;
	std::vector<Light>		lights;		// points and balls, no triangles
	bool					has_held = false;	// something in it is carried by the eye (PT_MAT_HELD)
	uint32_t				hash = 0;	// changes when anything in it does
};

struct Scene
{
	static constexpr float kCausticMax = 4.0f;		// the most that waves may brighten light by
	static constexpr float kRippleSlope = 0.1f;	// of each train of fine ripples on a liquid, at their most
	static constexpr float kWaterIndex = 1.33f;

	const World	*world = nullptr;
	const Frame	*frame = nullptr;
	const float	*light_styles = nullptr;
	int			num_light_styles = 0;
	int			anim_frame = 0;
	float		time = 0.0f;

	// the sky box turns about an axis
	Vec3		sky_axis{0, 0, 1};
	float		sky_sin = 0.0f, sky_cos = 1.0f;

	Vec3 Turn(Vec3 v, float s) const	// about sky_axis, by the angle whose sine is s
	{
		return v * sky_cos + Cross(sky_axis, v) * s + sky_axis * (Dot(sky_axis, v) * (1.0f - sky_cos));
	}
	Vec3 Sky(Vec3 world_dir) const { return world->Sky(Turn(world_dir, -sky_sin)); }
	Vec3 FromSky(Vec3 sky_dir) const { return Turn(sky_dir, sky_sin); }

	// settings, see pt_view_t
	int			light_samples = 8;
	float		max_sample = 40.0f;
	float		wave_strength = 1.0f;
	float		wave_reach = 0.0f;
	float		water_shafts = 0.0f;	// how much liquids scatter the light in them
	float		water_wet = 0.0f;		// how wet the banks of simulated liquids show
	// simulated liquids are met where their waves stand, not at their triangles
	bool		swell = false;
	bool Swells(const Tri &t) const { return swell && t.mat->wave_map && std::fabs(t.n.z) > 0.99f; }
	bool		filter_textures = true;
	int			reflections = 2;
	int			view_mode = 0;
	float		metal_colour = 0.0f;	// see pt_view_t
	int			reflection_bounces = 3;
	float		reflection_rate = 1.0f;
	bool		refraction = true;
	float		fog_density = 0.0f;		// 0 = clear air
	int			fog_samples = 1;		// points along a view ray where the air's light is looked for

	const Tri &TriAt(uint32_t index) const
	{
		return (index & kDynamic) ? frame->tris[index & ~kDynamic] : world->tris[index];
	}

	// textures made with texture_create, by handle
	const std::vector<std::unique_ptr<Texture>> *handles = nullptr;

	const Texture *Map(int handle_plus_one) const
	{
		if (!handles || handle_plus_one <= 0 || handle_plus_one > (int)handles->size())
			return nullptr;
		return (*handles)[handle_plus_one - 1].get();
	}

	// where a point falls on a liquid's maps
	static void WaveCoord(const Material &m, Vec3 p, float &u, float &v)
	{
		u = (p.x - m.wave_rect[0]) * m.wave_rect[2];
		v = (p.y - m.wave_rect[1]) * m.wave_rect[3];
	}

	// is there liquid at p, of the body whose maps the material carries
	bool Wet(const Material &m, Vec3 p) const
	{
		const Texture *t = Map(m.caustic_map);
		if (!t)
			return true;
		float u, v;
		WaveCoord(m, p, u, v);
		if (u < 0.0f || v < 0.0f || u >= 1.0f || v >= 1.0f)
			return false;
		return (t->pixels[(size_t)(v * (float)t->height) * t->width + (size_t)(u * (float)t->width)] >> 24) >= 128;
	}

	// lumps of about one unit across, 0 to 1, the same wherever it is asked
	static float Lumps(float x, float y)
	{
		const float fx = std::floor(x), fy = std::floor(y);
		float ax = x - fx, ay = y - fy;
		const int ix = (int)fx, iy = (int)fy;
		ax = ax * ax * (3.0f - 2.0f * ax);
		ay = ay * ay * (3.0f - 2.0f * ay);
		const float c00 = (float)(Hash((uint32_t)ix, (uint32_t)iy) >> 8), c10 = (float)(Hash((uint32_t)(ix + 1), (uint32_t)iy) >> 8);
		const float c01 = (float)(Hash((uint32_t)ix, (uint32_t)(iy + 1)) >> 8), c11 = (float)(Hash((uint32_t)(ix + 1), (uint32_t)(iy + 1)) >> 8);
		return ((c00 * (1.0f - ax) + c10 * ax) * (1.0f - ay) + (c01 * (1.0f - ax) + c11 * ax) * ay) * (1.0f / 16777216.0f);
	}

	// Bubbles packed together, about one unit across: 1 on the walls between
	// them, falling to 0 in their middles
	static float Bubbles(float x, float y)
	{
		const float cx = std::floor(x), cy = std::floor(y);
		const float fx = x - cx, fy = y - cy;
		const int ix = (int)cx, iy = (int)cy;
		float d1 = 8.0f, d2 = 8.0f;
		for (int j = -1; j <= 1; j++)
		{
			for (int i = -1; i <= 1; i++)
			{
				const uint32_t h = Hash((uint32_t)(ix + i), (uint32_t)(iy + j));
				const float ox = (float)i + (float)(h & 0xffffu) * (1.0f / 65535.0f) - fx;
				const float oy = (float)j + (float)(h >> 16) * (1.0f / 65535.0f) - fy;
				const float d = ox * ox + oy * oy;
				if (d < d1)
				{
					d2 = d1;
					d1 = d;
				}
				else if (d < d2)
					d2 = d;
			}
		}
		return 1.0f - std::min(std::max((std::sqrt(d2) - std::sqrt(d1)) * 2.2f, 0.0f), 1.0f);
	}

	// froth's own shape: large bubbles and small, most where their walls meet
	static float FrothPattern(float x, float y)
	{
		return 0.6f * Bubbles(x * 0.11f, y * 0.11f) + 0.4f * Bubbles(x * 0.37f, y * 0.37f);
	}

	// How much of the surface at p froth covers, 0 to 0.9, on the liquid
	// whose maps the material carries, and how old it is there. Thick and
	// fresh it lies closed; thinner or older it opens into rings and strings
	// along the walls of its bubbles. Froth in pt/rtx/shaders/scene.glsl is
	// the same and is to be kept so.
	float Froth(const Material &m, Vec3 p, float &age) const
	{
		const float slide = 1.6f;	// seconds the pattern rides on the liquid before it starts again
		const Texture *t = Map(m.foam_map);
		if (!t)
			return 0.0f;
		float u, v;
		WaveCoord(m, p, u, v);
		if (u < 0.0f || v < 0.0f || u > 1.0f || v > 1.0f)
			return 0.0f;
		uint32_t texel[4];
		float w[4], foam[4] = {};
		t->Corners(u, v, texel, w);
		for (int k = 0; k < 4; k++)
			for (int c = 0; c < 4; c++)
				foam[c] += (float)((texel[k] >> (c * 8)) & 0xff) * w[k];
		const float cover = foam[0] * (1.0f / 255.0f);
		age = foam[1] * (1.0f / 255.0f);
		if (cover <= 0.004f)
			return 0.0f;
		const float fx = foam[2] - 128.0f, fy = foam[3] - 128.0f;
		float pattern;
		if (fx * fx + fy * fy > 4.0f)
		{
			// The liquid carries it along. The pattern is drawn twice, each
			// sliding with the liquid for a while before it starts again,
			// and one fades in as the other fades out.
			float phase = time * (1.0f / slide);
			phase -= std::floor(phase);
			const float other = phase < 0.5f ? phase + 0.5f : phase - 0.5f;
			const float a = FrothPattern(p.x - fx * (slide * (other - 0.5f)), p.y - fy * (slide * (other - 0.5f)));
			const float b = FrothPattern(p.x - fx * (slide * (phase - 0.5f)), p.y - fy * (slide * (phase - 0.5f)));
			const float share = 1.0f - std::fabs(2.0f * phase - 1.0f);
			pattern = a * (1.0f - share) + b * share;
		}
		else
			pattern = FrothPattern(p.x, p.y);
		return std::min(std::max((pattern - 1.0f + cover * (1.3f - 0.45f * age)) * 3.0f, 0.0f), 1.0f) * 0.9f;
	}

	// Ripples too fine for a simulated liquid's cells to hold, where it is
	// as ruffled as can be: a few trains of small waves, each running its
	// own way at the speed waves of its length do. How sharply they curve
	// the surface at x, y: along x, along y, and across the two.
	Vec3 RippleCurve(float x, float y) const
	{
		Vec3 c(0.0f, 0.0f, 0.0f);
		for (int i = 0; i < 6; i++)
		{
			const float turn = (float)i * 2.399f + 0.5f;
			const float dx = std::cos(turn), dy = std::sin(turn);
			const float k = 0.28f + 0.11f * (float)i;	// from 22 units long down to 7
			// a wave whose slope is at most kRippleSlope curves by that times k
			const float bend = std::sin((dx * x + dy * y) * k - std::sqrt(386.0f * k) * time + (float)i * 1.7f) * (kRippleSlope * k);
			c = c - Vec3(dx * dx, dy * dy, dx * dy) * bend;
		}
		return c;
	}

	// How much the waves of a simulated liquid brighten light that goes
	// through its surface at q, or dim it: where the surface bulges it
	// gathers the light, as a lens does, and where it is hollow it spreads
	// it. m carries its maps; reach is how far the light goes on from
	// there, times how much the surface turns it for its slope, which is
	// 1 - 1 / the liquid's index. wet is whether there is liquid at q at all.
	float Caustic(const Material &m, Vec3 q, float reach, bool &wet) const
	{
		const Texture *t = Map(m.caustic_map);
		float u, v;
		WaveCoord(m, q, u, v);
		wet = t && u >= 0.0f && v >= 0.0f && u <= 1.0f && v <= 1.0f;
		if (!wet)
			return 1.0f;
		// clamped at the edges, as the card's sampler is
		const float fx = std::min(std::max(u * (float)t->width - 0.5f, 0.0f), (float)(t->width - 1));
		const float fy = std::min(std::max(v * (float)t->height - 0.5f, 0.0f), (float)(t->height - 1));
		const int x0 = std::min((int)fx, t->width - 2), y0 = std::min((int)fy, t->height - 2);
		const float ax = fx - (float)x0, ay = fy - (float)y0;
		const uint32_t *row = &t->pixels[(size_t)y0 * t->width + x0];
		const uint32_t texel[4] = {row[0], row[1], row[t->width], row[t->width + 1]};
		const float w[4] = {(1.0f - ax) * (1.0f - ay), ax * (1.0f - ay), (1.0f - ax) * ay, ax * ay};
		float sum[4] = {};
		for (int k = 0; k < 4; k++)
			for (int c = 0; c < 4; c++)
				sum[c] += (float)((texel[k] >> (8 * c)) & 0xff) * w[k];
		wet = sum[3] > 0.3f * 255.0f;
		// how sharply it curves along x and along y, and how ruffled it is: see pt_water_caustics
		const float sx = (sum[0] - 128.0f) * (1.0f / 127.0f), sy = (sum[1] - 128.0f) * (1.0f / 127.0f);
		const Vec3 c = (Vec3(sx * std::fabs(sx) * 0.125f, sy * std::fabs(sy) * 0.125f, 0.0f)
			+ RippleCurve(q.x, q.y) * (sum[2] * (4.0f / 255.0f))) * (reach * wave_strength);
		const float gain = 1.0f / std::max(std::fabs((1.0f + c.x) * (1.0f + c.y) - c.z * c.z), 0.01f);
		return std::min(std::max(gain, 0.2f), kCausticMax);
	}

	float StyleScale(int style) const
	{
		return (style > 0 && style < num_light_styles) ? light_styles[style] : 1.0f;
	}
};

std::unique_ptr<World> BuildWorld(const pt_world_t *in);
void BuildFrame(Frame &f, const pt_scene_t *in, const std::vector<std::unique_ptr<Texture>> &textures);
uint32_t HashBytes(const void *data, size_t bytes, uint32_t h);

} // namespace PT_NS
