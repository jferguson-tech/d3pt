// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Jonathan Ferguson

#include "pt_world.h"

#include <algorithm>
#include <cfloat>
#include <utility>

namespace PT_NS {

float g_to_linear[256];

void InitColourTables()
{
	static bool done = false;
	if (done)
		return;
	done = true;
	for (int i = 0; i < 256; i++)
		g_to_linear[i] = std::pow(i / 255.0f, 2.2f);
}

void Texture::Set(const pt_texture_t &src)
{
	width = src.width;
	height = src.height;
	const size_t count = (size_t)width * height;
	pixels.assign(src.pixels, src.pixels + count);

	Vec3 sum;
	for (size_t p = 0; p < count; p++)
		sum += Decode(pixels[p]);
	if (count)
		average = Max(sum / (float)count, Vec3(1e-4f));
}

void Material::Set(const pt_material_t &src, const Texture *tex, const Texture *normal_tex, const Texture *emission_tex)
{
	emission_map = (emission_tex && emission_tex->width > 0 && emission_tex->height > 0) ? emission_tex : nullptr;
	texture = (tex && tex->width > 0 && tex->height > 0) ? tex : nullptr;
	normal_texture = (normal_tex && normal_tex->width > 0 && normal_tex->height > 0) ? normal_tex : nullptr;
	emission = Vec3(src.emission);
	alpha = src.alpha;
	emission_seen = src.emission_seen;
	wave_map = src.wave_map;
	caustic_map = src.caustic_map;
	foam_map = src.foam_map;
	for (int k = 0; k < 4; k++)
		wave_rect[k] = src.wave_rect[k];
	absorb = Vec3(src.absorb);
	scroll_u = src.scroll[0];
	scroll_v = src.scroll[1];
	roughness = src.roughness < 0.0f ? 0.0f : (src.roughness > 1.0f ? 1.0f : src.roughness);
	metallic = src.metallic < 0.0f ? 0.0f : (src.metallic > 1.0f ? 1.0f : src.metallic);
	flags = src.flags;
	emissive = MaxComponent(emission) > 0.0f && !(flags & PT_MAT_SKY);
	emission_per_texel = emission;
	// where a map says which part of the surface the light comes from, the
	// light is shared out over that part and not over the whole picture
	const Texture *shared = (emission_map && (flags & PT_MAT_EMIT_MAPPED)) ? emission_map : texture;
	if (shared && !(flags & PT_MAT_EMIT_TEXTURE))
	{
		const Vec3 avg = shared->average;
		emission_per_texel = Vec3(emission.x / avg.x, emission.y / avg.y, emission.z / avg.z);
	}
}

void Tri::Set(Vec3 a, Vec3 b, Vec3 c)
{
	p0 = a;
	e1 = b - a;
	e2 = c - a;
	const Vec3 x = Cross(e1, e2);
	const float len = Length(x);
	area = len * 0.5f;
	n = len > 0.0f ? x / len : Vec3(0, 0, 1);
	smooth = false;

	// which way the texture runs across the surface, for normal maps
	const float du1 = uv[1][0] - uv[0][0], dv1 = uv[1][1] - uv[0][1];
	const float du2 = uv[2][0] - uv[0][0], dv2 = uv[2][1] - uv[0][1];
	const float det = du1 * dv2 - du2 * dv1;
	if (std::fabs(det) > 1e-12f)
	{
		tu = Normalize(e1 * dv2 - e2 * dv1) * (det < 0.0f ? -1.0f : 1.0f);
		tv = Normalize(e2 * du1 - e1 * du2) * (det < 0.0f ? -1.0f : 1.0f);
	}
	else
		Basis(n, tu, tv);
}

Vec3 World::Sky(Vec3 d) const
{
	const float ax = std::fabs(d.x), ay = std::fabs(d.y), az = std::fabs(d.z);
	int a = 0;
	if (ay > ax) a = 1;
	if (az > (a ? ay : ax)) a = 2;
	const int b = (a + 1) % 3, c = (a + 2) % 3;
	const float m = std::fabs(d[a]);
	const int face = a * 2 + (d[a] < 0.0f ? 1 : 0);
	if (sky[face] < 0 || m <= 0.0f)
		return Vec3(0, 0, 0);
	return textures[sky[face]].SampleClamped((d[b] / m + 1.0f) * 0.5f, (d[c] / m + 1.0f) * 0.5f) * sky_scale;
}

namespace {

void BuildLightGrid(World &w, const std::vector<float> &power)
{
	LightGrid &g = w.grid;
	if (w.lights.empty() || w.tris.empty())
		return;

	Vec3 lo(FLT_MAX, FLT_MAX, FLT_MAX), hi(-FLT_MAX, -FLT_MAX, -FLT_MAX);
	for (const Tri &t : w.tris)
	{
		const Vec3 p[3] = {t.p0, t.p0 + t.e1, t.p0 + t.e2};
		for (const Vec3 &v : p)
		{
			lo = Min(lo, v);
			hi = Max(hi, v);
		}
	}

	// about 40000 cells, but never finer than a doorway
	const Vec3 extent = hi - lo;
	float cell = std::cbrt(std::max(1.0f, extent.x * extent.y * extent.z) / 40000.0f);
	if (cell < 96.0f)
		cell = 96.0f;
	g.origin = lo;
	g.inv_cell = 1.0f / cell;
	for (int a = 0; a < 3; a++)
		g.dims[a] = std::max(1, (int)std::ceil(extent[a] * g.inv_cell));

	const size_t cells = (size_t)g.dims[0] * g.dims[1] * g.dims[2];
	const int k = LightGrid::kPerCell;
	g.light.assign(cells * k, 0);
	g.pdf.assign(cells * k, 0.0f);
	g.cdf.assign(cells * k, 1.0f);
	g.count.assign(cells, 0);

	const float min_dist2 = cell * cell * 0.75f;	// half the cell diagonal, squared
	std::vector<std::pair<float, uint32_t>> scored(w.lights.size());

	for (size_t ci = 0; ci < cells; ci++)
	{
		const size_t cx = ci % g.dims[0], cy = (ci / g.dims[0]) % g.dims[1], cz = ci / ((size_t)g.dims[0] * g.dims[1]);
		const Vec3 centre = lo + Vec3(cx + 0.5f, cy + 0.5f, cz + 0.5f) * cell;

		for (size_t li = 0; li < w.lights.size(); li++)
		{
			const Vec3 d = w.lights[li].origin - centre;
			scored[li] = {power[li] / std::max(Dot(d, d), min_dist2), (uint32_t)li};
		}
		const size_t keep = std::min((size_t)k, scored.size());
		std::partial_sort(scored.begin(), scored.begin() + keep, scored.end(),
			[](const std::pair<float, uint32_t> &a, const std::pair<float, uint32_t> &b) { return a.first > b.first; });

		float total = 0.0f;
		for (size_t i = 0; i < keep; i++)
			total += scored[i].first;
		if (total <= 0.0f)
			continue;

		float run = 0.0f;
		for (size_t i = 0; i < keep; i++)
		{
			g.light[ci * k + i] = scored[i].second;
			g.pdf[ci * k + i] = scored[i].first / total;
			run += scored[i].first;
			g.cdf[ci * k + i] = run / total;
		}
		g.cdf[ci * k + keep - 1] = 1.0f;
		g.count[ci] = (uint8_t)keep;
	}
}

} // namespace

Vec3 World::SampleSky(Rng &rng, float &pdf) const
{
	pdf = 0.0f;
	if (sky_cdf.empty() || sky_total <= 0.0f)
		return Vec3(0, 0, 1);

	const size_t pick = std::min((size_t)(std::upper_bound(sky_cdf.begin(), sky_cdf.end(), rng.Float()) - sky_cdf.begin()),
		sky_cdf.size() - 1);
	const int face = (int)(pick / ((size_t)sky_res * sky_res));
	const int texel = (int)(pick % ((size_t)sky_res * sky_res));
	const float u = (texel % sky_res + rng.Float()) / sky_res, v = (texel / sky_res + rng.Float()) / sky_res;

	const int a = face / 2, b = (a + 1) % 3, c = (a + 2) % 3;
	Vec3 d;
	d[a] = (face & 1) ? -1.0f : 1.0f;
	d[b] = 2.0f * u - 1.0f;
	d[c] = 2.0f * v - 1.0f;
	d = Normalize(d);

	// texels were weighted by luminance times solid angle, so per unit solid
	// angle the chance is just the luminance over its integral
	pdf = Luminance(Sky(d)) / sky_total;
	return d;
}

namespace {

// What the tree with eight children to a node is told of each triangle, so
// that it need not ask about the plain ones: which have holes in them, where
// their texture says, and which a ray gets past by chance. Empty where there
// is no such tree.
std::vector<uint8_t> Marks(const std::vector<Tri> &tris)
{
	std::vector<uint8_t> marks;
#ifdef PT_AVX2_KERNELS
	marks.resize(tris.size());
	for (size_t i = 0; i < tris.size(); i++)
	{
		const Material &m = *tris[i].mat;
		if (m.alpha < 1.0f)
			marks[i] = Bvh::kAsk | Bvh::kChancy;
		else	// a simulated liquid is not met at its triangles: see Scene::Swells
			marks[i] = (((m.flags & PT_MAT_ALPHA_TEST) && m.texture) || m.wave_map) ? Bvh::kAsk : 0;
	}
#else
	(void)tris;
#endif
	return marks;
}

void BuildSkyLight(World &w)
{
	w.sky_cdf.clear();
	w.sky_total = 0.0f;
	w.sky_res = 0;
	for (int f = 0; f < 6; f++)
		if (w.sky[f] < 0)
			return;

	const int res = 64;
	w.sky_res = res;
	w.sky_cdf.resize((size_t)6 * res * res);
	double total = 0.0;
	for (int f = 0; f < 6; f++)
	{
		const int a = f / 2, b = (a + 1) % 3, c = (a + 2) % 3;
		for (int y = 0; y < res; y++)
		{
			for (int x = 0; x < res; x++)
			{
				Vec3 d;
				d[a] = (f & 1) ? -1.0f : 1.0f;
				d[b] = 2.0f * (x + 0.5f) / res - 1.0f;
				d[c] = 2.0f * (y + 0.5f) / res - 1.0f;
				const float len2 = Dot(d, d);
				const float solid = (4.0f / (res * res)) / (len2 * std::sqrt(len2));
				total += Luminance(w.Sky(Normalize(d))) * solid;
				w.sky_cdf[((size_t)f * res + y) * res + x] = (float)total;
			}
		}
	}
	if (total <= 0.0)
	{
		w.sky_cdf.clear();
		return;
	}
	for (float &v : w.sky_cdf)
		v = (float)(v / total);
	w.sky_cdf.back() = 1.0f;
	w.sky_total = (float)total;
}

// From each light grid cell, how much of the sky's light gets in. Indoors
// that is none, and looking for it there every time would be a waste.
void BuildSkyChance(World &w)
{
	w.sky_chance.clear();
	const LightGrid &g = w.grid;
	if (w.sky_cdf.empty() || g.count.empty())
		return;

	const size_t cells = (size_t)g.dims[0] * g.dims[1] * g.dims[2];
	w.sky_chance.assign(cells, 0.0f);
	const float cell = 1.0f / g.inv_cell;
	Rng rng(12345);

	for (size_t ci = 0; ci < cells; ci++)
	{
		const size_t cx = ci % g.dims[0], cy = (ci / g.dims[0]) % g.dims[1], cz = ci / ((size_t)g.dims[0] * g.dims[1]);
		int reached = 0;
		const int points = 5, dirs = 6;
		for (int p = 0; p < points; p++)
		{
			const Vec3 at = g.origin + Vec3(cx + (p ? rng.Float() : 0.5f), cy + (p ? rng.Float() : 0.5f),
				cz + (p ? rng.Float() : 0.5f)) * cell;
			for (int k = 0; k < dirs; k++)
			{
				float pdf;
				Ray ray;
				ray.o = at;
				ray.d = w.SampleSky(rng, pdf);
				ray.tmin = 0.0f;
				ray.tmax = FLT_MAX;
				Hit hit = {};
				// glass and water let the sky through
				if (w.bvh.IntersectIf(ray, hit, [&](uint32_t t, float, float) { return w.tris[t].mat->alpha >= 1.0f; })
					&& (w.tris[hit.tri].mat->flags & PT_MAT_SKY))
					reached++;
			}
		}
		const float fraction = (float)reached / (points * dirs);
		w.sky_chance[ci] = fraction > 0.0f ? std::min(1.0f, std::max(0.25f, fraction * 4.0f)) : 0.0f;
	}

	// a cell next to one that sees the sky may well see some too
	std::vector<float> spread(w.sky_chance);
	for (size_t ci = 0; ci < cells; ci++)
	{
		if (w.sky_chance[ci] > 0.0f)
			continue;
		const int cx = (int)(ci % g.dims[0]), cy = (int)((ci / g.dims[0]) % g.dims[1]), cz = (int)(ci / ((size_t)g.dims[0] * g.dims[1]));
		float best = 0.0f;
		for (int dz = -1; dz <= 1; dz++)
			for (int dy = -1; dy <= 1; dy++)
				for (int dx = -1; dx <= 1; dx++)
				{
					const int x = cx + dx, y = cy + dy, z = cz + dz;
					if (x < 0 || y < 0 || z < 0 || x >= g.dims[0] || y >= g.dims[1] || z >= g.dims[2])
						continue;
					best = std::max(best, w.sky_chance[((size_t)z * g.dims[1] + y) * g.dims[0] + x]);
				}
		spread[ci] = best > 0.0f ? 0.1f : 0.0f;
	}
	w.sky_chance.swap(spread);
}

} // namespace

std::unique_ptr<World> BuildWorld(const pt_world_t *in)
{
	std::unique_ptr<World> w(new World);

	w->textures.resize(in->num_textures);
	for (int i = 0; i < in->num_textures; i++)
		w->textures[i].Set(in->textures[i]);

	const auto texture = [&](int t) { return (t >= 0 && t < in->num_textures) ? &w->textures[t] : nullptr; };

	w->materials.resize(std::max(1, in->num_materials));
	for (int i = 0; i < in->num_materials; i++)
	{
		Material &m = w->materials[i];
		m.Set(in->materials[i], texture(in->materials[i].texture), texture(in->materials[i].normal_texture),
			texture(in->materials[i].emission_texture - 1));
		// glowing detail is too dim and too patchy to be worth sampling as a light
		m.sampled = m.emissive && !(m.flags & PT_MAT_EMIT_BRIGHT) && (!m.emission_map || (m.flags & PT_MAT_EMIT_MAPPED));
		if (m.flags & PT_MAT_WAVES)
			w->has_waves = true;
	}
	for (int i = 0; i < in->num_materials; i++)
	{
		const int next = in->materials[i].anim_next;
		if (next >= 0 && next < in->num_materials && next != i)
			w->materials[i].anim_next = &w->materials[next];
	}
	for (Material &m : w->materials)
	{
		// animations loop; count the steps until this one comes round again
		int length = 1;
		for (const Material *n = m.anim_next; n && n != &m && length < 64; n = n->anim_next)
			length++;
		m.anim_length = length;
	}

	std::vector<Vec3> soup((size_t)in->num_triangles * 3);
	w->tris.resize(in->num_triangles);
	for (int i = 0; i < in->num_triangles; i++)
	{
		Tri &t = w->tris[i];
		for (int k = 0; k < 3; k++)
		{
			const uint32_t vi = in->indices[i * 3 + k];
			soup[(size_t)i * 3 + k] = Vec3(&in->positions[vi * 3]);
			t.uv[k][0] = in->uvs[vi * 2];
			t.uv[k][1] = in->uvs[vi * 2 + 1];
		}
		t.Set(soup[(size_t)i * 3], soup[(size_t)i * 3 + 1], soup[(size_t)i * 3 + 2]);
		const uint32_t m = in->tri_materials[i];
		t.mat = &w->materials[m < (uint32_t)in->num_materials ? m : 0];
	}
	w->bvh.Build(soup.data(), (uint32_t)in->num_triangles, Marks(w->tris).data());

	// The simulated bodies of liquid, for finding where their waves stand
	// and for the light they throw back up. A body is whatever shows one
	// wave picture, of however many materials its faces are made.
	for (const Tri &t : w->tris)
	{
		if (!t.mat->wave_map || std::fabs(t.n.z) < 0.99f)
			continue;
		World::Water *body = nullptr;
		for (World::Water &b : w->waters)
			if (b.mat->wave_map == t.mat->wave_map && std::fabs(b.z - t.p0.z) <= 1.0f)
				body = &b;
		if (!body)
		{
			w->waters.push_back({FLT_MAX, FLT_MAX, -FLT_MAX, -FLT_MAX, t.p0.z, t.mat, false, {}});
			body = &w->waters.back();
		}
		if (t.n.z > 0.0f)
			body->top = true;
		const Vec3 corner[3] = {t.p0, t.p0 + t.e1, t.p0 + t.e2};
		for (const Vec3 &c : corner)
		{
			body->min_x = std::min(body->min_x, c.x);
			body->min_y = std::min(body->min_y, c.y);
			body->max_x = std::max(body->max_x, c.x);
			body->max_y = std::max(body->max_y, c.y);
		}
	}

	// everything that emits, with a chance of being picked proportional to its power
	std::vector<float> power;
	for (int i = 0; i < in->num_triangles; i++)
	{
		const Tri &t = w->tris[i];
		if (!t.mat->sampled || t.area <= 1e-6f)
			continue;
		Light l;
		l.tri = (uint32_t)i;
		l.origin = t.p0 + (t.e1 + t.e2) * (1.0f / 3.0f);
		l.emission = t.mat->emission;
		l.pdf = 0;
		l.style = 0;
		l.cone_cos = 0.0f;
		w->lights.push_back(l);
		power.push_back(Luminance(t.mat->emission) * t.area * kPi);
	}
	for (int i = 0; i < in->num_lights; i++)
	{
		Light l;
		l.tri = ~0u;
		l.origin = Vec3(in->lights[i].origin);
		l.emission = Vec3(in->lights[i].intensity);
		l.pdf = 0;
		l.style = in->lights[i].style;
		l.dir = Vec3(in->lights[i].direction);
		l.cone_cos = in->lights[i].cone_cos;
		const float p = Luminance(l.emission) * 4.0f * kPi;
		if (p <= 0.0f)
			continue;
		w->lights.push_back(l);
		power.push_back(p);
	}

	double total = 0;
	for (float p : power)
		total += p;
	double run = 0;
	w->light_cdf.resize(power.size());
	for (size_t i = 0; i < power.size(); i++)
	{
		w->lights[i].pdf = (float)(power[i] / total);
		run += power[i];
		w->light_cdf[i] = (float)(run / total);
	}

	// Where the light on each simulated liquid comes from, for the patterns
	// its waves throw on what lies in it: over each part of it, the middle
	// of the lights that shine on its surface there, each counted for what
	// it puts there. A part no light shines on has what the rest has, and
	// with none at all it comes from overhead, as under an open sky.
	for (World::Water &b : w->waters)
	{
		float weights[World::Water::kLamps * World::Water::kLamps];
		Vec3 all(0.0f, 0.0f, 0.0f);
		float all_weight = 0.0f;
		for (int k = 0; k < World::Water::kLamps * World::Water::kLamps; k++)
		{
			const Vec3 at(b.min_x + (b.max_x - b.min_x) * (((float)(k % World::Water::kLamps) + 0.5f) / (float)World::Water::kLamps),
				b.min_y + (b.max_y - b.min_y) * (((float)(k / World::Water::kLamps) + 0.5f) / (float)World::Water::kLamps), b.z + 1.0f);
			Vec3 sum(0.0f, 0.0f, 0.0f);
			float weight = 0.0f;
			for (const Light &l : w->lights)
			{
				const Vec3 d = l.origin - at;
				const float dist2 = Dot(d, d);
				if (d.z <= 1.0f || dist2 > 2048.0f * 2048.0f)
					continue;
				const Vec3 wi = d * (1.0f / std::sqrt(dist2));
				float e = Luminance(l.emission) * wi.z / dist2;
				if (l.tri != ~0u)
					e *= std::max(-Dot(w->tris[l.tri].n, wi), 0.0f) * w->tris[l.tri].area;
				else if (l.cone_cos > 0.0f && -Dot(wi, l.dir) < l.cone_cos)
					continue;
				if (e <= 0.0f)
					continue;
				Ray ray;
				ray.o = at;
				ray.d = d;
				ray.tmin = 0.0f;
				ray.tmax = 0.999f;
				if (w->bvh.AnyHit(ray, [&](uint32_t i, float, float) { return i != l.tri && w->tris[i].mat->alpha >= 1.0f; }))
					continue;
				sum = sum + l.origin * e;
				weight += e;
			}
			weights[k] = weight;
			if (weight > 0.0f)
			{
				b.lamps[k] = sum * (1.0f / weight);
				all = all + b.lamps[k];
				all_weight += 1.0f;
			}
		}
		for (int k = 0; k < World::Water::kLamps * World::Water::kLamps; k++)
			if (weights[k] <= 0.0f)
				b.lamps[k] = all_weight > 0.0f ? all * (1.0f / all_weight)
					: Vec3((b.min_x + b.max_x) * 0.5f, (b.min_y + b.max_y) * 0.5f, b.z + 4096.0f);
	}
	if (!w->light_cdf.empty())
		w->light_cdf.back() = 1.0f;
	BuildLightGrid(*w, power);

	for (int i = 0; i < 6; i++)
		w->sky[i] = (in->sky_textures[i] >= 0 && in->sky_textures[i] < in->num_textures) ? in->sky_textures[i] : -1;
	w->sky_scale = in->sky_scale;
	BuildSkyLight(*w);
	BuildSkyChance(*w);
	return w;
}

uint32_t HashBytes(const void *data, size_t bytes, uint32_t h)
{
	const uint8_t *p = (const uint8_t *)data;
	for (size_t i = 0; i < bytes; i++)
		h = (h ^ p[i]) * 16777619u;
	return h;
}

void BuildFrame(Frame &f, const pt_scene_t *in, const std::vector<std::unique_ptr<Texture>> &textures)
{
	f.materials.clear();
	f.tris.clear();
	f.prev.clear();
	f.lights.clear();
	f.has_held = false;
	f.hash = 2166136261u;

	std::vector<Vec3> soup;
	if (in)
	{
		const auto texture = [&](int t)
		{
			return (t >= 0 && t < (int)textures.size() && textures[t]) ? textures[t].get() : nullptr;
		};

		f.materials.resize(std::max(1, in->num_materials));
		for (int i = 0; i < in->num_materials; i++)
		{
			Material &m = f.materials[i];
			m.Set(in->materials[i], texture(in->materials[i].texture), texture(in->materials[i].normal_texture),
				texture(in->materials[i].emission_texture - 1));
			m.sampled = m.emissive && (m.flags & PT_MAT_SAMPLED);
		}

		f.tris.resize(in->num_triangles);
		soup.resize((size_t)in->num_triangles * 3);
		for (int i = 0; i < in->num_triangles; i++)
		{
			Tri &t = f.tris[i];
			for (int k = 0; k < 3; k++)
			{
				soup[(size_t)i * 3 + k] = Vec3(&in->positions[i * 9 + k * 3]);
				t.uv[k][0] = in->uvs[i * 6 + k * 2];
				t.uv[k][1] = in->uvs[i * 6 + k * 2 + 1];
			}
			t.Set(soup[(size_t)i * 3], soup[(size_t)i * 3 + 1], soup[(size_t)i * 3 + 2]);
			if (in->normals)
			{
				for (int k = 0; k < 3; k++)
					t.vn[k] = Vec3(&in->normals[i * 9 + k * 3]);
				t.smooth = true;
			}
			const uint32_t m = in->tri_materials[i];
			t.mat = &f.materials[m < (uint32_t)in->num_materials ? m : 0];
			if (t.mat->flags & PT_MAT_HELD)
				f.has_held = true;
		}

		for (int i = 0; i < in->num_lights; i++)
		{
			Light l;
			l.tri = ~0u;
			l.origin = Vec3(in->lights[i].origin);
			l.emission = Vec3(in->lights[i].intensity);
			l.pdf = 0;
			l.style = 0;
			l.cone_cos = 0.0f;
			l.radius = in->lights[i].radius > 0.0f ? in->lights[i].radius : 0.0f;
			if (Luminance(l.emission) > 0.0f)
				f.lights.push_back(l);
		}

		if (in->prev_positions)
		{
			f.prev.resize((size_t)in->num_triangles * 3);
			for (size_t k = 0; k < f.prev.size(); k++)
				f.prev[k] = Vec3(&in->prev_positions[k * 3]);
		}

		f.hash = HashBytes(in->positions, (size_t)in->num_triangles * 9 * sizeof(float), f.hash);
		f.hash = HashBytes(in->materials, (size_t)in->num_materials * sizeof(pt_material_t), f.hash);
		f.hash = HashBytes(in->lights, (size_t)in->num_lights * sizeof(pt_point_light_t), f.hash);
	}
	f.bvh.Build(soup.data(), (uint32_t)f.tris.size(), Marks(f.tris).data());
}

} // namespace PT_NS
