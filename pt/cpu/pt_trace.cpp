// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Jonathan Ferguson

#include "pt_trace.h"
#include "../material/pt_material.h"

#include <algorithm>
#include <cfloat>
#include <cstring>

namespace PT_NS {

namespace {

const float kGlobalLightChance = 0.2f;	// how often a light is picked map wide instead of nearby
const float kMinAlpha = 0.002f;			// GGX gets numerically touchy below this

void TexCoord(const Tri &t, float u, float v, float &s, float &tt)
{
	const float b0 = 1.0f - u - v;
	s = t.uv[0][0] * b0 + t.uv[1][0] * u + t.uv[2][0] * v;
	tt = t.uv[0][1] * b0 + t.uv[1][1] * u + t.uv[2][1] * v;
}

bool IsHole(const Tri &t, float u, float v)
{
	if (!(t.mat->flags & PT_MAT_ALPHA_TEST) || !t.mat->texture)
		return false;
	float s, tt;
	TexCoord(t, u, v, s, tt);
	return (t.mat->texture->Texel(s, tt) >> 24) < 128;
}

// is anything in the way of light along the ray
static bool Blocked(const Scene &sc, const Ray &shadow, Rng &rng)
{
	const auto blocks = [&](const Tri &t, float u, float v)
	{
		if (IsHole(t, u, v) || BackOfGlass(t, shadow.d))
			return false;
		if (t.mat->alpha >= 1.0f || rng.Float() < t.mat->alpha)
			return true;
		return false;
	};

#ifdef PT_AVX2_KERNELS
	// The trees with eight children answer sooner, and say the same or that
	// they are unsure, as when a surface that light gets through by chance is
	// in the way: see Bvh::AnyHit8. The trees below are then asked, as they
	// always were.
	const auto kind = [&](const Tri &t, float u, float v)
	{
		if (IsHole(t, u, v) || BackOfGlass(t, shadow.d))
			return 0;
		return t.mat->alpha >= 1.0f ? 1 : 2;
	};
	const int world = sc.world->bvh.AnyHit8(shadow, [&](uint32_t i, float u, float v) { return kind(sc.world->tris[i], u, v); });
	if (world == 1)
		return true;
	if (world == 0)
	{
		const int frame = sc.frame->bvh.AnyHit8(shadow, [&](uint32_t i, float u, float v) { return kind(sc.frame->tris[i], u, v); });
		if (frame == 1)
			return true;
		if (frame == 0)
			return false;
		// nothing of the world's was in the way, so only the frame is left to ask
		if (sc.frame->bvh.AnyHit(shadow, [&](uint32_t i, float u, float v) { return blocks(sc.frame->tris[i], u, v); }))
			return true;
		return false;
	}
#endif

	if (sc.world->bvh.AnyHit(shadow, [&](uint32_t i, float u, float v) { return blocks(sc.world->tris[i], u, v); }))
		return true;
	if (sc.frame->bvh.AnyHit(shadow, [&](uint32_t i, float u, float v) { return blocks(sc.frame->tris[i], u, v); }))
		return true;
	return false;
}

// Light bends where it goes into a liquid. Of a path with one end in the
// liquid and the other out of it, whose surface lies level at z: where it
// crosses, which is nearer over the end in the liquid than a straight line
// between the two would.
static Vec3 BentCrossing(Vec3 in_liquid, Vec3 out_of_it, float z)
{
	const float down = z - in_liquid.z, up = out_of_it.z - z;
	const float ax = out_of_it.x - in_liquid.x, ay = out_of_it.y - in_liquid.y;
	const float len = std::sqrt(ax * ax + ay * ay);
	if (len < 1.0e-4f)
		return Vec3(in_liquid.x, in_liquid.y, z);
	// the sines of the two angles at the surface are as the liquid's index:
	// from where that puts it for small angles, and nearer each step
	const float straight = len * down / (down + up);
	float x = len * down / (down + Scene::kWaterIndex * up);
	for (int i = 0; i < 3; i++)
	{
		const float y = len - x;
		const float a = 1.0f / std::sqrt(x * x + down * down), b = 1.0f / std::sqrt(y * y + up * up);
		x -= (Scene::kWaterIndex * x * a - y * b) / (Scene::kWaterIndex * down * down * a * a * a + up * up * b * b * b);
		x = std::min(std::max(x, 0.0f), straight);
	}
	const float share = x / len;
	return Vec3(in_liquid.x + ax * share, in_liquid.y + ay * share, z);
}

// How much of the light from target reaches the surface: 0 if something is
// in the way, otherwise 1. With bend, a little less where the path crosses
// the surface of a simulated liquid: there it bends, as light does going
// into water, so a shadow falls where the bent light is cut off. That is for
// what the eye sees itself; light that has bounced takes the liquid for no
// more than it looks. (What the waves do to the light is laid over what the
// eye sees: see AddWater.)
float Visible(const Scene &sc, const Surface &s, Vec3 target, Rng &rng, bool bend = false)
{
	rng.rays++;
	Ray shadow;
	shadow.o = s.p + s.ng * kRayOffset;
	shadow.d = target - shadow.o;
	shadow.tmin = 0.0f;
	shadow.tmax = 0.999f;

	if (bend && sc.swell)
	{
		for (const World::Water &b : sc.world->waters)
		{
			if ((shadow.o.z - b.z) * (target.z - b.z) >= 0.0f)
				continue;
			const bool rising = target.z > b.z;
			const Vec3 out = rising ? target : shadow.o;
			const Vec3 q = BentCrossing(rising ? shadow.o : target, out, b.z);
			if (q.x < b.min_x || q.x > b.max_x || q.y < b.min_y || q.y > b.max_y || !sc.Map(b.mat->caustic_map) || !sc.Wet(*b.mat, q))
				continue;
			// as far as the surface, and from there on
			Ray leg = shadow;
			leg.d = q - shadow.o;
			if (Blocked(sc, leg, rng))
				return 0.0f;
			leg.o = q;
			leg.d = target - q;
			leg.tmin = 1.0e-3f;
			rng.rays++;
			if (Blocked(sc, leg, rng))
				return 0.0f;
			// less of it gets in at a glancing angle, and what is painted on
			// the liquid stops its share
			const float m = 1.0f - (out.z - b.z) / Length(out - q);
			return (0.98f - 0.98f * m * m * m * m * m) * (b.top ? 1.0f - b.mat->alpha : 1.0f);
		}
	}
	return Blocked(sc, shadow, rng) ? 0.0f : 1.0f;
}

// ---- GGX microfacet reflection, height correlated Smith shadowing ----

Vec3 Fresnel(Vec3 f0, float voh)
{
	const float m = 1.0f - (voh < 0.0f ? 0.0f : voh);
	const float m5 = m * m * m * m * m;
	return f0 + (Vec3(1, 1, 1) - f0) * m5;
}

float SmithLambda(float alpha, float nox)	// the square root term shared by G1 and G2
{
	return std::sqrt(alpha * alpha + (1.0f - alpha * alpha) * nox * nox);
}

// the specular lobe's value times the cosine at the light: f * (n.wi)
Vec3 SpecularTimesCos(const Surface &s, Vec3 wi)
{
	const float nol = Dot(s.n, wi), nov = Dot(s.n, s.wo);
	if (nol <= 0.0f || nov <= 0.0f)
		return Vec3();
	const Vec3 h = Normalize(s.wo + wi);
	const float noh = Dot(s.n, h), voh = Dot(s.wo, h);
	const float a2 = s.alpha * s.alpha;
	const float d = noh * noh * (a2 - 1.0f) + 1.0f;
	const float ndf = a2 / (kPi * d * d);
	const float vis = 0.5f / (nol * SmithLambda(s.alpha, nov) + nov * SmithLambda(s.alpha, nol));
	return Fresnel(s.f0, voh) * (ndf * vis * nol);
}

// Light arriving from direction wi with radiance-times-solid-angle e
// (irradiance on a surface facing it), through each lobe
Lit Reflect(const Surface &s, Vec3 wi, Vec3 e)
{
	Lit out;
	if (s.medium)
	{
		out.diffuse = e;		// air has no facing
		return out;
	}
	const float nol = Dot(s.n, wi);
	if (nol <= 0.0f)
		return out;
	out.diffuse = e * nol;
	if (s.light_sampled_spec)
		out.specular = e * SpecularTimesCos(s, wi);
	return out;
}

float Importance(const Surface &s, const Lit &l)
{
	return Luminance(s.kd * l.diffuse) * kInvPi + Luminance(l.specular);
}

// a point light's contribution, ignoring occlusion
Lit PointLight(const Surface &s, const Light &l, float scale)
{
	const Vec3 d = l.origin - s.p;
	const float dist2 = Dot(d, d);
	if (dist2 <= 1e-6f || scale <= 0.0f)
		return Lit();
	return Reflect(s, d / std::sqrt(dist2), l.emission * (scale / dist2));
}

// A point on a ball of light, drawn evenly over as much of the ball as shows
// from p: y is the point and wi the way to it. Returns what it sends to p
// over the chance of its being drawn, which comes to the same wherever on
// the ball it is: the ball's radiance times the solid angle the ball fills.
// From inside it, nothing.
Vec3 SampleBall(const Light &l, Vec3 p, Rng &rng, Vec3 &y, Vec3 &wi)
{
	const Vec3 d = l.origin - p;
	const float dist2 = Dot(d, d), r2 = l.radius * l.radius;
	if (dist2 <= r2)
		return Vec3();
	const float dist = std::sqrt(dist2);
	const float sin2_max = r2 / dist2;
	// 1 - cos of the angle from its middle to its edge, which far off is too small to take from 1
	const float cap = sin2_max / (1.0f + std::sqrt(1.0f - sin2_max));

	const float cos_t = 1.0f - rng.Float() * cap, phi = 2.0f * kPi * rng.Float();
	const float sin2_t = std::max(0.0f, 1.0f - cos_t * cos_t), sin_t = std::sqrt(sin2_t);
	Vec3 t, b;
	const Vec3 w = d / dist;
	Basis(w, t, b);
	wi = t * (sin_t * std::cos(phi)) + b * (sin_t * std::sin(phi)) + w * cos_t;
	// where going that way meets the ball
	y = p + wi * (dist * cos_t - std::sqrt(std::max(0.0f, r2 - dist2 * sin2_t)));
	return l.emission * (2.0f * cap / r2);		// intensity / (pi r^2) * 2 pi cap
}

// One of the frame's lights on a surface, with nothing in the way; at is
// where to look to see whether anything is. A point gives its all, a ball
// what one point drawn on it stands for.
Lit FrameLight(const Surface &s, const Light &l, Rng &rng, Vec3 &at)
{
	if (l.radius <= 0.0f)
	{
		at = l.origin;
		return PointLight(s, l, 1.0f);
	}
	Vec3 wi;
	const Vec3 e = SampleBall(l, s.p, rng, at, wi);
	return Reflect(s, wi, e);
}

// How much one of the frame's lights is worth to a surface beside the
// others. For a point that is what it gives; a ball is weighed as if it were
// all at its middle, but for where the middle is under the surface's horizon
// and some of the ball over it: there it is not to be left out.
float FrameLightWeight(const Surface &s, const Light &l)
{
	if (l.radius <= 0.0f || s.medium)
		return Importance(s, PointLight(s, l, 1.0f));
	const Vec3 d = l.origin - s.p;
	const float dist2 = Dot(d, d);
	if (dist2 <= l.radius * l.radius)
		return 0.0f;
	const float dist = std::sqrt(dist2);
	const Vec3 wi = d / dist, e = l.emission * (1.0f / dist2);
	const float rise = l.radius / dist, nol = Dot(s.n, wi);
	if (nol <= -rise)
		return 0.0f;
	float weight = Luminance((s.kd * kInvPi + s.f0 * 0.05f) * e) * std::max(nol, 0.25f * (nol + rise));
	if (s.light_sampled_spec)
		weight += Luminance(e * SpecularTimesCos(s, wi));
	return weight;
}

} // namespace

bool BackOfGlass(const Tri &tri, Vec3 dir)
{
	// sprites, beams and particles are flat things meant to be seen from both sides
	return tri.mat->alpha < 1.0f && !(tri.mat->flags & (PT_MAT_BLACK | PT_MAT_EMIT_TEXTURE))
		&& Dot(tri.n, dir) > 0.0f;
}

bool Finite(float f)
{
	uint32_t bits;
	memcpy(&bits, &f, sizeof(bits));
	return (bits & 0x7f800000u) != 0x7f800000u;
}

Vec3 ClampSample(Vec3 c, float max_luminance)
{
	const float lum = Luminance(c);
	if (!Finite(lum))
		return Vec3();
	return lum > max_luminance ? c * (max_luminance / lum) : c;
}

// how far above its level a simulated liquid stands at x, y
static float WaveHeight(const Scene &sc, const Texture &map, const Material &m, float x, float y)
{
	// between the middles of the cells, and level with the outermost beyond them
	const float fx = std::min(std::max((x - m.wave_rect[0]) * m.wave_rect[2] * (float)map.width - 0.5f, 0.0f), (float)(map.width - 1));
	const float fy = std::min(std::max((y - m.wave_rect[1]) * m.wave_rect[3] * (float)map.height - 0.5f, 0.0f), (float)(map.height - 1));
	const int x0 = std::min((int)fx, map.width - 2), y0 = std::min((int)fy, map.height - 2);
	const float ax = fx - (float)x0, ay = fy - (float)y0;
	const uint32_t *row = &map.pixels[(size_t)y0 * map.width + x0];
	const auto stored = [](uint32_t texel) { return (float)(((texel >> 8) & 0xff00) | (texel >> 24)); };
	const float h = (stored(row[0]) * (1.0f - ax) + stored(row[1]) * ax) * (1.0f - ay)
		+ (stored(row[map.width]) * (1.0f - ax) + stored(row[map.width + 1]) * ax) * ay;
	return (h * (1.0f / 65535.0f) - 0.5f) * 16.0f * sc.wave_strength;
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
void AddWater(const Scene &sc, Surface &s, bool under)
{
	if (!sc.swell)
		return;
	for (const World::Water &b : sc.world->waters)
	{
		const Material &m = *b.mat;
		const float up = s.p.z - b.z;
		const Texture *waves = sc.Map(m.wave_map), *marks = sc.Map(m.caustic_map);
		if (up > 12.0f || (!under && up < -9.0f) || !waves || !marks
			|| s.p.x < b.min_x - 8.0f || s.p.y < b.min_y - 8.0f || s.p.x > b.max_x + 8.0f || s.p.y > b.max_y + 8.0f)
			continue;

		// how high it has stood, between the cells that have liquid in or by them
		const float fx = (s.p.x - m.wave_rect[0]) * m.wave_rect[2] * (float)marks->width - 0.5f;
		const float fy = (s.p.y - m.wave_rect[1]) * m.wave_rect[3] * (float)marks->height - 0.5f;
		const int x0 = (int)std::floor(fx), y0 = (int)std::floor(fy);
		const float ax = fx - (float)x0, ay = fy - (float)y0;
		float sum = 0.0f, weight = 0.0f;
		for (int k = 0; k < 4; k++)
		{
			const int x = x0 + (k & 1), y = y0 + (k >> 1);
			if (x < 0 || y < 0 || x >= marks->width || y >= marks->height)
				continue;
			const float v = (float)(marks->pixels[(size_t)y * marks->width + x] >> 24);
			const float w = ((k & 1) ? ax : 1.0f - ax) * ((k >> 1) ? ay : 1.0f - ay);
			if (v > 127.5f)
			{
				sum += v * w;
				weight += w;
			}
		}
		if (weight <= 0.0f)
			continue;

		if (up < WaveHeight(sc, *waves, m, s.p.x, s.p.y))
		{
			// in it, if the eye's ray is; something under its bed is not
			const Vec3 lamp = b.Lamp(s.p.x, s.p.y);
			if (!under || lamp.z <= b.z || up >= 0.0f)
				continue;
			const Vec3 q = BentCrossing(s.p, lamp, b.z);
			const float down = Length(q - s.p), from = Length(q - lamp);
			bool wet;
			const float gain = sc.Caustic(m, q, (1.0f - 1.0f / Scene::kWaterIndex) * down * from / (down + from), wet);
			// what faces away from the light has none of it
			const float facing = std::min(std::max(Dot(s.n, q - s.p) / down * 4.0f, 0.0f), 1.0f);
			if (wet)
				s.kd = s.kd * (1.0f + (gain - 1.0f) * facing);
			return;
		}
		if (sc.water_wet <= 0.0f)
			continue;

		const float mark = (sum / weight - 160.0f) * (8.0f / 95.0f) * sc.wave_strength;
		// it soaks a little higher than it stood, and not to a ruled line
		const float edge = mark + 0.75f + 1.5f * Scene::Lumps((s.p.x + s.p.y) * 0.45f, (s.p.z + s.p.x - s.p.y) * 0.45f);
		const float wet = std::min(std::max((edge - up) * 1.5f, 0.0f), 1.0f) * std::min(sc.water_wet, 1.0f);
		if (wet <= 0.0f)
			continue;

		// water fills what made the surface rough and pale
		const float dark = 1.0f - 0.45f * wet;
		s.colour = s.colour * dark;
		s.kd = s.kd * dark;
		s.roughness = std::min(s.roughness, s.roughness + (0.3f - s.roughness) * wet);
		s.alpha = std::max(s.roughness * s.roughness, kMinAlpha);
		s.light_sampled_spec = s.roughness >= kLightSampledRoughness;
		return;
	}
}

// Where the ray first crosses the surface of a simulated liquid before limit.
// The surface is not the level triangles the map has for it but stands where
// the waves do, above and below them: the ray is followed through the band
// the waves keep to until it is on the other side of the surface. Gives how
// far along that is, the side the ray came from (1 above, -1 below) and the
// body.
static bool WaveCross(const Scene &sc, const Ray &ray, float limit, float &where, float &side, const World::Water *&body)
{
	const float reach = sc.wave_reach * sc.wave_strength + 0.1f;
	const float o[3] = {ray.o.x, ray.o.y, ray.o.z}, d[3] = {ray.d.x, ray.d.y, ray.d.z};
	float inv[3];
	for (int a = 0; a < 3; a++)
		inv[a] = 1.0f / (std::fabs(d[a]) < 1.0e-8f ? 1.0e-8f : d[a]);
	limit = std::min(limit, 1.0e7f);
	bool found = false;

	for (const World::Water &b : sc.world->waters)
	{
		// the part of the ray within the waves' reach of the level, over the body
		const float lo[3] = {b.min_x, b.min_y, b.z - reach}, hi[3] = {b.max_x, b.max_y, b.z + reach};
		float t0 = ray.tmin, t1 = limit;
		for (int a = 2; a >= 0 && t0 < t1; a--)
		{
			const float ta = (lo[a] - o[a]) * inv[a], tb = (hi[a] - o[a]) * inv[a];
			t0 = std::max(t0, std::min(ta, tb));
			t1 = std::min(t1, std::max(ta, tb));
		}
		if (t0 >= t1)
			continue;
		const Texture *map = sc.Map(b.mat->wave_map), *wet = sc.Map(b.mat->caustic_map);
		if (!map || map->width < 2 || map->height < 2)
			continue;

		const auto above = [&](float t)
		{
			return o[2] + d[2] * t - b.z - WaveHeight(sc, *map, *b.mat, o[0] + d[0] * t, o[1] + d[1] * t);
		};
		// Far from the surface the steps are as long as its slope allows,
		// taken to be no steeper than one in one; near it half a cell. A ray
		// that skims it for long gets longer steps as it goes, so as to end.
		const float cell = 1.0f / (b.mat->wave_rect[2] * (float)map->width);
		const float across = std::sqrt(d[0] * d[0] + d[1] * d[1]);
		const float closing = 1.0f / (std::fabs(d[2]) + across);
		float least = 0.5f * cell / std::max(across, 1.0e-6f);

		float ta = t0, fa = above(t0);
		// a ray that leaves the surface starts on it, and a little further
		// along shows which side it left on
		if (std::fabs(fa) < 0.05f)
			fa = above(std::min(t0 + 0.1f, t1));
		for (int i = 0; ta < t1; i++)
		{
			if (i >= 64)
				least *= 1.06f;
			const float tb = std::min(ta + std::max(std::fabs(fa) * closing, least), t1);
			const float fb = above(tb);
			if ((fa < 0.0f) != (fb < 0.0f))
			{
				// the caustic picture also says where there is liquid at all
				const float tm = 0.5f * (ta + tb);
				const int cx = std::min(std::max((int)std::floor((o[0] + d[0] * tm - b.mat->wave_rect[0]) * b.mat->wave_rect[2] * (float)map->width), 0), map->width - 1);
				const int cy = std::min(std::max((int)std::floor((o[1] + d[1] * tm - b.mat->wave_rect[1]) * b.mat->wave_rect[3] * (float)map->height), 0), map->height - 1);
				if (!wet || wet->width != map->width || wet->height != map->height || (wet->pixels[(size_t)cy * wet->width + cx] >> 24) >= 128)
				{
					float on = ta, past = tb;
					for (int k = 0; k < 10; k++)
					{
						const float mid = 0.5f * (on + past);
						if ((above(mid) < 0.0f) == (fa < 0.0f))
							on = mid;
						else
							past = mid;
					}
					where = on;
					side = fa < 0.0f ? -1.0f : 1.0f;
					body = &b;
					limit = on;
					found = true;
					break;
				}
			}
			ta = tb;
			fa = fb;
		}
	}
	return found;
}

// A ray that crosses the surface of a simulated liquid before limit meets
// the triangle that says there is liquid at the crossing and what it is made
// of: the one straight down from it, or up, from the side the ray came.
static bool WaveHit(const Scene &sc, const Ray &ray, float limit, Hit &hit)
{
	float where, side;
	const World::Water *body;
	if (!WaveCross(sc, ray, limit, where, side, body))
		return false;

	Ray probe;
	probe.o = Vec3(ray.o.x + ray.d.x * where, ray.o.y + ray.d.y * where, body->z + side * 1.25f);
	probe.d = Vec3(0.0f, 0.0f, -side);
	probe.tmin = 0.0f;
	probe.tmax = 2.5f;
	Hit at;
	if (!sc.world->bvh.IntersectIf(probe, at, [&](uint32_t t, float, float)
	{
		const Tri &tri = sc.world->tris[t];
		return tri.mat->wave_map == body->mat->wave_map && tri.n.z * side > 0.99f;
	}))
		return false;
	hit = at;
	hit.t = where;
	return true;
}

bool Closest(const Scene &sc, Ray &ray, Rng &rng, bool camera, bool cross, Hit &hit, const Tri *&tri, HeldRays held, bool waves)
{
	waves = waves && sc.swell && held != kHeldOnly;
	rng.rays++;
	for (int skips = 0; ; skips++)
	{
		const auto in_world = [&](uint32_t t, float, float)
		{
			return !BackOfGlass(sc.world->tris[t], ray.d) && !(waves && sc.Swells(sc.world->tris[t]));
		};
		const auto in_frame = [&](uint32_t t, float, float)
		{
			const Tri &tri = sc.frame->tris[t];
			if (held != kHeldToo && ((tri.mat->flags & PT_MAT_HELD) != 0) != (held == kHeldOnly))
				return false;
			return !BackOfGlass(tri, ray.d);
		};
		bool found = false;
#ifdef PT_AVX2_KERNELS
		bool unsure = false;
#endif
		if (held != kHeldOnly)
		{
#ifdef PT_AVX2_KERNELS
			// The trees with eight children find the same triangle sooner, or
			// say that they cannot be sure of it: see Bvh::Intersect8. The
			// trees below are then asked, as they always were.
			found = sc.world->bvh.Intersect8(ray, hit, in_world, unsure);
			if (unsure)
				found = sc.world->bvh.IntersectIf(ray, hit, in_world);
#else
			found = sc.world->bvh.IntersectIf(ray, hit, in_world);
#endif
		}
		Ray r = ray;
		if (found)
			r.tmax = hit.t;
		Hit h;
		bool found_frame;
		if (held != kHeldToo)
		{
			// The tree with eight children is not asked about plain
			// triangles, and what the eye carries is not marked out to it
			// (that would cost every ray while playing, when it is not
			// kept apart): the frame's tree is small, so ask it about all
			// of them.
			found_frame = sc.frame->bvh.IntersectIf(r, h, in_frame);
		}
		else
		{
#ifdef PT_AVX2_KERNELS
			found_frame = sc.frame->bvh.Intersect8(r, h, in_frame, unsure);
			if (unsure)
				found_frame = sc.frame->bvh.IntersectIf(r, h, in_frame);
#else
			found_frame = sc.frame->bvh.IntersectIf(r, h, in_frame);
#endif
		}
		if (found_frame)
		{
			hit = h;
			hit.tri |= kDynamic;
			found = true;
		}
		// a simulated liquid's surface, if that comes first
		if (waves && WaveHit(sc, ray, found ? hit.t : ray.tmax, hit))
			found = true;
		if (!found)
			return false;

		tri = &sc.TriAt(hit.tri);
		const Material &m = *tri->mat;
		const bool skip = skips < 32 && (
			(camera && (m.flags & PT_MAT_CAMERA_INVISIBLE)) ||
			IsHole(*tri, hit.u, hit.v) ||
			(cross && m.alpha < 1.0f && rng.Float() >= m.alpha) ||
			(sc.view_mode == PT_VIEW_FURNACE && (m.flags & PT_MAT_BLACK)) ||
			// a ray that leaves the liquid where it stands below its level is
			// behind the level sheet, and must not meet that from the back
			(!waves && sc.Swells(*tri) && Dot(tri->n, ray.d) > 0.0f));
		if (!skip)
			return true;
		ray.tmin = hit.t + 0.01f;
	}
}

// Epic's fit to the specular lobe's total reflectance (Karis, "Physically
// Based Shading on Mobile")
Vec3 Surface::SpecularAlbedo() const
{
	const float nov = std::max(0.0f, Dot(n, wo));
	const float r0 = -roughness + 1.0f;
	const float r1 = -0.0275f * roughness + 0.0425f;
	const float r2 = -0.572f * roughness + 1.04f;
	const float r3 = 0.022f * roughness - 0.04f;
	const float a004 = std::min(r0 * r0, std::exp2(-9.28f * nov)) * r0 + r1;
	const float scale = -1.04f * a004 + r2, bias = 1.04f * a004 + r3;
	return f0 * scale + Vec3(bias);
}

bool ViewSolid(int mode, const Material &mat)
{
	if (mode == PT_VIEW_CLAY)
		return mat.alpha >= 1.0f || (mat.flags & PT_MAT_WAVES);
	return mode == PT_VIEW_FURNACE;
}

// A view mode's say over what the surface is made of (pt_view_t's view_mode).
// What the surface emits is worked out from its colour, which clay and
// mirror leave alone.
static void ViewMode(int mode, const Material &mat, Surface &s)
{
	if (mode == PT_VIEW_CLAY && ViewSolid(mode, mat))
	{
		s.kd = Vec3(0.5f);
		s.f0 = Vec3(0.04f);
		s.roughness = 1.0f;
	}
	else if (mode == PT_VIEW_MIRROR)
		s.roughness = 0.0f;
	else if (mode == PT_VIEW_FURNACE)
	{
		s.kd = Vec3(1.0f);
		s.f0 = Vec3(0.04f);
		s.roughness = 1.0f;
		s.colour = Vec3();		// and nothing glows
		s.glow = Vec3();
	}
}

void WhiteSurface(Surface &s)
{
	if (s.mat->flags & PT_MAT_BLACK)
		return;
	s.kd = Vec3(1.0f - s.metallic);
	s.f0 = Vec3(0.04f * (1.0f - s.metallic) + s.metallic);
}

Vec3 SurfaceChannel(int mode, const Surface &s)
{
	// shown as the number it is: the display's curve undoes this one
	const auto plain = [](float v) { return std::pow(std::min(std::max(v, 0.0f), 1.0f), 2.2f); };
	switch (mode)
	{
	case PT_VIEW_BASE_COLOUR:
		return s.colour;
	case PT_VIEW_NORMALS:
		return Vec3(plain(0.5f + 0.5f * s.n.x), plain(0.5f + 0.5f * s.n.y), plain(0.5f + 0.5f * s.n.z));
	case PT_VIEW_ROUGHNESS:
		return Vec3(plain(s.roughness));
	case PT_VIEW_METAL:
		return Vec3(plain(s.metallic));
	case PT_VIEW_GLOW:
	{
		if (!s.mat->emissive || !s.front)
			return Vec3();
		const Vec3 e = Emitted(s, true);
		return e * (1.0f / std::max(1.0f, MaxComponent(e)));
	}
	default:
		return Vec3();
	}
}

// black, blue, green, yellow and red at 0 to 4, cyan between blue and green
static Vec3 CountColour(float at)
{
	static const Vec3 ramp[] = {Vec3(0, 0, 0), Vec3(0, 0, 1), Vec3(0, 1, 1), Vec3(0, 1, 0), Vec3(1, 1, 0), Vec3(1, 0, 0)};
	// cyan is a stop of its own, half way from 1 to 2
	at = std::min(std::max(at, 0.0f), 4.0f);
	const float stop = at < 1.0f ? at : at < 2.0f ? 1.0f + (at - 1.0f) * 2.0f : at + 1.0f;
	const int below = std::min((int)stop, 4);
	return ramp[below] + (ramp[below + 1] - ramp[below]) * (stop - (float)below);
}

Vec3 BounceColour(float bounces)
{
	return CountColour(bounces);
}

Vec3 CostColour(float rays)
{
	// a colour for each doubling from PT_COST_BLUE up, and a fade to black below it
	const float at = rays / PT_COST_BLUE;
	return CountColour(at < 1.0f ? at : 1.0f + std::log2(at));
}

// what a metal reflects, worked out from the colour it was painted (PT_MAT_METAL_PAINTED)
static Vec3 MetalColour(Vec3 c, float level)
{
	const float painted[3] = {c.x, c.y, c.z};
	float reflects[3];
	pt_material_metal_colour(painted, level, reflects);
	return Vec3(reflects[0], reflects[1], reflects[2]);
}

void MakeSurface(const Scene &sc, const Tri &tri, const Hit &hit, const Ray &ray, Surface &s, bool smooth, bool with_froth)
{
	const Material &mat = tri.mat->At(sc.anim_frame);
	s.tri = &tri;
	s.mat = &mat;
	s.medium = false;
	s.foam = 0.0f;
	s.cover = mat.alpha;
	s.p = ray.o + ray.d * hit.t;
	s.wo = -ray.d;
	s.front = Dot(tri.n, ray.d) < 0.0f;
	s.ng = s.front ? tri.n : -tri.n;

	float u, v;
	TexCoord(tri, hit.u, hit.v, u, v);
	if (mat.flags & PT_MAT_WARP)
	{
		// each coordinate sways with the other, an eighth of a repeat either way
		const float u0 = u;
		u += 0.125f * std::sin(v * 8.0f + sc.time);
		v += 0.125f * std::sin(u0 * 8.0f + sc.time);
	}
	u += mat.scroll_u * sc.time;
	v += mat.scroll_v * sc.time;

	// a simulated surface: the wave picture holds its slopes and height
	const bool simulated = mat.wave_map && sc.wave_strength > 0.0f && sc.Map(mat.wave_map);
	float wave_x = 0.0f, wave_y = 0.0f, wave_height = 0.0f;
	if (simulated)
	{
		float wu, wv;
		Scene::WaveCoord(mat, s.p, wu, wv);
		uint32_t texel[4];
		float w[4];
		sc.Map(mat.wave_map)->Corners(wu, wv, texel, w);
		for (int k = 0; k < 4; k++)
		{
			wave_x += (float)(texel[k] & 0xff) * w[k];
			wave_y += (float)((texel[k] >> 8) & 0xff) * w[k];
			wave_height += (float)(((texel[k] >> 8) & 0xff00) | (texel[k] >> 24)) * w[k];
		}
		wave_x = (wave_x * (1.0f / 255.0f) - 0.5f) * sc.wave_strength;
		wave_y = (wave_y * (1.0f / 255.0f) - 0.5f) * sc.wave_strength;
		wave_height = (wave_height * (1.0f / 65535.0f) - 0.5f) * 16.0f * sc.wave_strength;
		// what the texture stands for lies below the surface, so a tilted
		// surface shows it shifted, as through a lens
		u += wave_x * 0.5f;
		v += wave_y * 0.5f;
	}

	s.colour = !mat.texture ? Vec3(1, 1, 1) : (smooth ? mat.texture->Smooth(u, v) : Decode(mat.texture->Texel(u, v)));
	if (mat.emission_map)
		s.glow = smooth ? mat.emission_map->Smooth(u, v) : Decode(mat.emission_map->Texel(u, v));
	s.roughness = mat.roughness;
	float metallic = mat.metallic;

	Vec3 n = s.ng;
	if (tri.smooth)
	{
		n = Normalize(tri.vn[0] * (1.0f - hit.u - hit.v) + tri.vn[1] * hit.u + tri.vn[2] * hit.v);
		if (!s.front)
			n = -n;
	}
	if (mat.normal_texture)
	{
		// x, y, z and roughness, each 0-255
		float c[4];
		if (smooth)
		{
			uint32_t t[4];
			float w[4];
			mat.normal_texture->Corners(u, v, t, w);
			for (int k = 0; k < 4; k++)
				c[k] = (float)((t[0] >> (k * 8)) & 0xff) * w[0] + (float)((t[1] >> (k * 8)) & 0xff) * w[1]
					+ (float)((t[2] >> (k * 8)) & 0xff) * w[2] + (float)((t[3] >> (k * 8)) & 0xff) * w[3];
		}
		else
		{
			const uint32_t t = mat.normal_texture->Texel(u, v);
			for (int k = 0; k < 4; k++)
				c[k] = (float)((t >> (k * 8)) & 0xff);
		}
		const float tx = c[0] * (2.0f / 255.0f) - 1.0f;
		const float ty = c[1] * (2.0f / 255.0f) - 1.0f;
		float tz = c[2] * (2.0f / 255.0f) - 1.0f;
		if (mat.flags & PT_MAT_METAL_TEXTURE)
		{
			// the third number is metal: z is what is left of the normal's length
			tz = std::sqrt(std::max(1.0f - tx * tx - ty * ty, 0.01f));
			metallic = c[2] * (1.0f / 255.0f);
		}
		n = Normalize(tri.tu * tx + tri.tv * ty + n * tz);
		s.roughness = c[3] * (1.0f / 255.0f);
	}
	if (simulated)
	{
		// the surface is level, facing up or (seen from below) down
		n = Normalize(Vec3(-wave_x, -wave_y, 1.0f)) * (n.z < 0.0f ? -1.0f : 1.0f);
		if (sc.Swells(tri))
		{
			// met where the waves stand (see WaveCross), on the side this
			// triangle faces: it really is tilted as they are
			n = Normalize(Vec3(-wave_x, -wave_y, 1.0f)) * (tri.n.z < 0.0f ? -1.0f : 1.0f);
			s.front = true;
			s.ng = n;
		}
		// crests gather the light in the liquid and troughs spread it
		s.colour *= std::min(std::max(1.0f + wave_height * 0.35f, 0.6f), 1.8f);

		// Froth is air and liquid finely mixed, which scatters light every
		// way: a pale matt layer over the liquid, which still shows through
		// where the layer is thin and still shines a little where it is not.
		// Bubbles do not lie flat.
		float age = 0.0f;
		const float froth = with_froth ? sc.Froth(mat, s.p, age) : 0.0f;
		if (froth > 0.0f)
		{
			const Vec3 pale = (s.colour * 0.4f + Vec3(0.6f)) * ((0.55f - 0.2f * age) * (0.7f + 0.3f * Scene::Lumps(s.p.x * 2.3f, s.p.y * 2.3f)));
			const float both = mat.alpha + froth - mat.alpha * froth;
			s.colour = (s.colour * (mat.alpha * (1.0f - froth)) + pale * froth) * (1.0f / both);
			s.foam = froth;
			s.cover = both;
			n = Normalize(n + Vec3(Scene::Lumps(s.p.x * 1.9f, s.p.y * 1.9f) - 0.5f,
				Scene::Lumps(s.p.y * 1.9f + 31.7f, s.p.x * 1.9f + 31.7f) - 0.5f, 0.0f) * (0.5f * froth));
			s.glow = Vec3();
		}
	}
	else if ((mat.flags & PT_MAT_WAVES) && sc.wave_strength > 0.0f)
	{
		// a few crossing ripples, enough to break up a reflection
		const float t = sc.time;
		const float a = std::sin(s.p.x * 0.071f + t * 1.7f) + std::sin(s.p.y * 0.053f - t * 1.3f)
			+ 0.5f * std::sin((s.p.x + s.p.y) * 0.19f + t * 2.9f);
		const float b = std::sin(s.p.y * 0.067f + t * 1.5f) + std::sin(s.p.x * 0.047f - t * 1.9f)
			+ 0.5f * std::sin((s.p.x - s.p.y) * 0.17f + t * 2.3f);
		Vec3 t1, t2;
		Basis(n, t1, t2);
		n = Normalize(n + t1 * (a * 0.02f * sc.wave_strength) + t2 * (b * 0.02f * sc.wave_strength));
	}
	// a normal tilted away from the viewer is no use for shading
	if (Dot(n, s.wo) < 0.02f || Dot(n, s.ng) <= 0.0f)
		n = s.ng;
	s.n = n;

	s.metallic = metallic;
	if ((mat.flags & PT_MAT_BLACK) && s.foam <= 0.0f)
	{
		s.kd = Vec3();
		s.f0 = Vec3();
	}
	else
	{
		s.kd = s.colour * (1.0f - metallic);
		s.f0 = Vec3(0.04f) * (1.0f - metallic)
			+ ((mat.flags & PT_MAT_METAL_PAINTED) ? MetalColour(s.colour, sc.metal_colour) : s.colour) * metallic;
		if (sc.view_mode)
			ViewMode(sc.view_mode, mat, s);
	}
	s.alpha = std::max(s.roughness * s.roughness, kMinAlpha);
	s.light_sampled_spec = s.roughness >= kLightSampledRoughness;
}

Vec3 Emitted(const Surface &s, bool seen)
{
	const Material &m = *s.mat;
	if (s.foam > 0.0f)
		return Vec3();
	if (m.emission_map)
	{
		if (!(m.flags & PT_MAT_EMIT_MAPPED))
			return m.emission * s.glow;
		// the material's own light, from where the map says it comes
		return (seen && m.emission_seen > 0.0f) ? s.glow * m.emission_seen : m.emission_per_texel * s.glow;
	}
	if (m.flags & PT_MAT_EMIT_BRIGHT)
	{
		// the lit parts of a screen or a button: bright texels glow, dark ones do not
		const float level = (MaxComponent(s.colour) - 0.12f) * (1.0f / 0.3f);
		const float t = level < 0.0f ? 0.0f : (level > 1.0f ? 1.0f : level);
		return m.emission * s.colour * (t * t * (3.0f - 2.0f * t));
	}
	return (seen && m.emission_seen > 0.0f) ? s.colour * m.emission_seen : m.emission_per_texel * s.colour;
}

// Light thrown back up by a simulated liquid surface, onto walls and
// ceilings near water. The surface is treated as a level mirror for the
// light just sampled, so its image lies as far below the surface as the
// light is above. y is the point on the light, e what it would send
// straight here.
static void WaterBounce(const Scene &sc, const Surface &s, Vec3 y, Vec3 e, Lit &out, Rng &rng)
{
	if (s.medium)
		return;
	// The body the light would come off: of those whose surface the path
	// crosses, the one nearest below. Only that one is tried, so that a map
	// with many bodies costs no more rays than one with few.
	const World::Water *body = nullptr;
	Vec3 q;		// where the path meets the surface
	for (const World::Water &b : sc.world->waters)
	{
		if (!b.top || s.p.z <= b.z + 1.0f || y.z <= b.z + 1.0f || s.p.z - b.z > 512.0f || (body && b.z <= body->z))
			continue;
		const Vec3 image(y.x, y.y, 2.0f * b.z - y.z);
		const Vec3 d = image - s.p;
		const Vec3 at = s.p + d * ((b.z - s.p.z) / d.z);
		if (at.x < b.min_x || at.x > b.max_x || at.y < b.min_y || at.y > b.max_y || !sc.Wet(*b.mat, at))
			continue;
		body = &b;
		q = at;
	}
	if (!body)
		return;

	const Vec3 d = Vec3(y.x, y.y, 2.0f * body->z - y.z) - s.p;
	const float len2 = Dot(d, d);
	const Vec3 wi = d * (1.0f / std::sqrt(len2));
	const float m = 1.0f + wi.z;	// 1 - cosine of the angle at the water
	const float fresnel = 0.02f + 0.98f * m * m * m * m * m;

	// e was for the straight path; this one is as long as the way to the image
	const Vec3 straight = y - s.p;
	const Lit add = Reflect(s, wi, e * (fresnel * Dot(straight, straight) / len2));
	if (Importance(s, add) <= 0.0f)
		return;

	// both legs must be clear
	const Vec3 above = q + Vec3(0.0f, 0.0f, 0.1f);
	if (Visible(sc, s, above, rng) <= 0.0f)
		return;
	Surface at{};
	at.medium = true;
	at.p = above;
	if (Visible(sc, at, y, rng) <= 0.0f)
		return;

	out.diffuse += add.diffuse;
	out.specular += add.specular;
}

#ifdef PT_AVX2_KERNELS

// Where in the tables a light is looked for, DirectLights finds the place
// with these in the AVX2 build. They come to the places std::upper_bound and
// a search along the list come to, without the branches those take.

// how many of the count sorted values come before the first above x
static size_t FirstAbove(const float *sorted, size_t count, float x)
{
	size_t at = 0;
	for (size_t n = count; n > 1; )
	{
		const size_t half = n / 2;
		at = sorted[at + half - 1] <= x ? at + half : at;
		n -= half;
	}
	return at + (count && sorted[at] <= x ? 1 : 0);
}

// the same of a cell's table, which has room for LightGrid::kPerCell, 24,
// values whatever count is: all are looked at at once
static_assert(LightGrid::kPerCell == 24, "the searches of a cell's tables take three lots of eight");

static int FirstAboveInCell(const float *sorted, int count, float x)
{
	const __m256 xs = _mm256_set1_ps(x);
	const unsigned below =
		(unsigned)_mm256_movemask_ps(_mm256_cmp_ps(_mm256_loadu_ps(sorted), xs, _CMP_LE_OQ))
		| (unsigned)_mm256_movemask_ps(_mm256_cmp_ps(_mm256_loadu_ps(sorted + 8), xs, _CMP_LE_OQ)) << 8
		| (unsigned)_mm256_movemask_ps(_mm256_cmp_ps(_mm256_loadu_ps(sorted + 16), xs, _CMP_LE_OQ)) << 16;
	// sorted, so those not above x are the first so many
#ifdef _MSC_VER
	return (int)__popcnt(below & ((1u << count) - 1));
#else
	return __builtin_popcount(below & ((1u << count) - 1));
#endif
}

// the last of the first count places of a cell's list of lights that holds
// this one, or -1
static int LastPlaceOf(const uint32_t *list, int count, uint32_t light)
{
	const __m256i want = _mm256_set1_epi32((int)light);
	unsigned found =
		(unsigned)_mm256_movemask_ps(_mm256_castsi256_ps(_mm256_cmpeq_epi32(_mm256_loadu_si256((const __m256i *)list), want)))
		| (unsigned)_mm256_movemask_ps(_mm256_castsi256_ps(_mm256_cmpeq_epi32(_mm256_loadu_si256((const __m256i *)(list + 8)), want))) << 8
		| (unsigned)_mm256_movemask_ps(_mm256_castsi256_ps(_mm256_cmpeq_epi32(_mm256_loadu_si256((const __m256i *)(list + 16)), want))) << 16;
	found &= (1u << count) - 1;
	if (!found)
		return -1;
#ifdef _MSC_VER
	return 31 - (int)_lzcnt_u32(found);
#else
	return 31 - __builtin_clz(found);
#endif
}

#endif	// PT_AVX2_KERNELS

static Lit DirectLights(const Scene &sc, const Surface &s, Rng &rng, bool first_hit)
{

	const int candidates = std::max(1, first_hit ? sc.light_samples : sc.light_samples / 2);
	const World &w = *sc.world;
	Lit none;
	if (w.lights.empty())
		return none;

	// Candidates are weighed by the light they would put on the surface, which
	// is cheap; how the surface reflects it is worked out for the winner only.
	float wsum = 0.0f;
	Vec3 chosen_y, chosen_wi, chosen_e;
	float chosen_phat = 0.0f;

	const LightGrid &g = w.grid;
	const int k = LightGrid::kPerCell;
	const size_t cell = g.count.empty() ? 0 : g.Cell(s.p);
	const int in_cell = g.count.empty() ? 0 : g.count[cell];
	const float *cell_cdf = in_cell ? &g.cdf[cell * k] : nullptr;
	const float use_global = in_cell ? kGlobalLightChance : 1.0f;

	for (int i = 0; i < candidates; i++)
	{
		// mostly from the list for this cell, sometimes from the whole map so nothing is missed
		size_t li;
		float cell_pdf = 0.0f;
		if (rng.Float() < use_global)
		{
#ifdef PT_AVX2_KERNELS
			li = std::min(FirstAbove(w.light_cdf.data(), w.light_cdf.size(), rng.Float()), w.lights.size() - 1);
			const int j = in_cell ? LastPlaceOf(&g.light[cell * k], in_cell, (uint32_t)li) : -1;
			if (j >= 0)
				cell_pdf = g.pdf[cell * k + j];
#else
			li = std::min((size_t)(std::upper_bound(w.light_cdf.begin(), w.light_cdf.end(), rng.Float())
				- w.light_cdf.begin()), w.lights.size() - 1);
			for (int j = 0; j < in_cell; j++)
				if (g.light[cell * k + j] == li)
					cell_pdf = g.pdf[cell * k + j];
#endif
		}
		else
		{
#ifdef PT_AVX2_KERNELS
			const int j = std::min(FirstAboveInCell(cell_cdf, in_cell, rng.Float()), in_cell - 1);
#else
			const int j = std::min((int)(std::upper_bound(cell_cdf, cell_cdf + in_cell, rng.Float()) - cell_cdf), in_cell - 1);
#endif
			li = g.light[cell * k + j];
			cell_pdf = g.pdf[cell * k + j];
		}
		const Light &l = w.lights[li];
		float pdf = use_global * l.pdf + (1.0f - use_global) * cell_pdf;

		Vec3 y, e;
		if (l.tri != ~0u)
		{
			const Tri &t = w.tris[l.tri];
			float a = rng.Float(), b = rng.Float();
			if (a + b > 1.0f) { a = 1.0f - a; b = 1.0f - b; }
			y = t.p0 + t.e1 * a + t.e2 * b;
			e = l.emission;
			pdf /= t.area;
		}
		else
		{
			y = l.origin;
			const float scale = sc.StyleScale(l.style);
			if (scale <= 0.0f)
				continue;
			e = l.emission * scale;
		}

		const Vec3 d = y - s.p;
		const float dist2 = Dot(d, d);
		if (dist2 <= 1e-6f)
			continue;
		const Vec3 wi = d * (1.0f / std::sqrt(dist2));
		if (l.cone_cos > 0.0f && l.tri == ~0u && -Dot(wi, l.dir) < l.cone_cos)
			continue;		// outside the spotlight's cone
		const float nol = s.medium ? 1.0f : Dot(s.n, wi);
		if (nol <= 0.0f)
			continue;
		float geom = 1.0f / dist2;
		if (l.tri != ~0u)
		{
			const float cosy = -Dot(w.tris[l.tri].n, wi);
			if (cosy <= 0.0f)
				continue;
			geom *= cosy;
		}
		e *= geom;

		const float phat = Luminance(e) * nol;
		if (phat <= 0.0f)
			continue;
		const float weight = phat / pdf;
		wsum += weight;
		if (rng.Float() * wsum < weight)
		{
			chosen_y = y;
			chosen_wi = wi;
			chosen_e = e;
			chosen_phat = phat;
		}
	}

	if (wsum <= 0.0f)
		return none;
	chosen_e *= wsum / (candidates * chosen_phat);

	Lit out;
	const float clear = Visible(sc, s, chosen_y, rng, first_hit);
	if (clear > 0.0f)
		out = Reflect(s, chosen_wi, chosen_e * clear);
	if (!sc.world->waters.empty())
		WaterBounce(sc, s, chosen_y, chosen_e, out, rng);
	return out;
}

// Light from the sky: a direction is drawn where the sky is bright and the
// light counts if nothing solid is in the way. Only tried as often as the
// sky can be seen from around here at all.
static Lit DirectSky(const Scene &sc, const Surface &s, Rng &rng)
{
	const World &w = *sc.world;
	Lit none;
	if (w.sky_cdf.empty())
		return none;

	float chance = 1.0f;
	if (!w.sky_chance.empty())
		chance = w.sky_chance[w.grid.Cell(s.p)];
	if (chance <= 0.0f || rng.Float() >= chance)
		return none;

	float pdf;
	const Vec3 sky_dir = w.SampleSky(rng, pdf);
	if (pdf <= 0.0f)
		return none;
	const Vec3 wi = sc.FromSky(sky_dir);
	if (!s.medium && (Dot(s.n, wi) <= 0.0f || Dot(s.ng, wi) <= 0.0f))
		return none;

	Ray ray;
	ray.o = s.p + s.ng * kRayOffset;
	ray.d = wi;
	ray.tmin = 0.0f;
	ray.tmax = FLT_MAX;
	Hit hit;
	const Tri *tri;
	if (!Closest(sc, ray, rng, false, true, hit, tri) || !(tri->mat->flags & PT_MAT_SKY))
		return none;

	return Reflect(s, wi, w.Sky(sky_dir) * (1.0f / (pdf * chance)));
}

Lit DirectWorld(const Scene &sc, const Surface &s, Rng &rng, bool first_hit)
{
	if (sc.view_mode == PT_VIEW_FURNACE)
		return Lit();		// no light is lit
	Lit lit = DirectLights(sc, s, rng, first_hit);
	const Lit sky = DirectSky(sc, s, rng);
	lit.diffuse += sky.diffuse;
	lit.specular += sky.specular;
	return lit;
}

Vec3 DirectMedium(const Scene &sc, Vec3 p, Rng &rng)
{
	Surface s{};
	s.medium = true;
	s.p = p;
	s.kd = Vec3(1, 1, 1);
	s.light_sampled_spec = false;
	const Lit world = DirectWorld(sc, s, rng, false), frame = DirectFrameOne(sc, s, rng);
	return world.diffuse + frame.diffuse;
}

// one of the frame's lights, or of its balls of light only
static Lit FrameOne(const Scene &sc, const Surface &s, Rng &rng, bool balls)
{

	const std::vector<Light> &lights = sc.frame->lights;
	Lit none;
	if (lights.empty() || sc.view_mode == PT_VIEW_FURNACE)
		return none;

	// picked in proportion to its unoccluded contribution: exactly, if a point
	float total = 0.0f;
	for (const Light &l : lights)
		if (!balls || l.radius > 0.0f)
			total += FrameLightWeight(s, l);
	if (total <= 0.0f)
		return none;

	float pick = rng.Float() * total;
	for (const Light &l : lights)
	{
		if (balls && l.radius <= 0.0f)
			continue;
		const float imp = FrameLightWeight(s, l);
		pick -= imp;
		if (pick <= 0.0f && imp > 0.0f)
		{
			Vec3 at;
			Lit f = FrameLight(s, l, rng, at);
			if (Importance(s, f) <= 0.0f)
				return none;
			const float clear = Visible(sc, s, at, rng);
			if (clear <= 0.0f)
				return none;
			f.diffuse *= clear * total / imp;
			f.specular *= clear * total / imp;
			return f;
		}
	}
	return none;
}

Lit DirectFrameOne(const Scene &sc, const Surface &s, Rng &rng)
{
	return FrameOne(sc, s, rng, false);
}

Lit DirectFrameBall(const Scene &sc, const Surface &s, Rng &rng)
{
	return FrameOne(sc, s, rng, true);
}

Lit DirectFrameAll(const Scene &sc, const Surface &s, Rng &rng)
{
	Lit sum;
	if (sc.view_mode == PT_VIEW_FURNACE)
		return sum;
	for (const Light &l : sc.frame->lights)
	{
		if (l.radius > 0.0f)
			continue;
		const Lit f = PointLight(s, l, 1.0f);
		const float clear = Importance(s, f) > 0.0f ? Visible(sc, s, l.origin, rng) : 0.0f;
		if (clear > 0.0f)
		{
			sum.diffuse += f.diffuse * clear;
			sum.specular += f.specular * clear;
		}
	}
	return sum;
}

float FrameLightLevel(const Scene &sc, const Surface &s, Vec3 flash_light, Vec3 eye, Vec3 dir, float reach)
{
	float level = Luminance(flash_light);
	for (const Light &l : sc.frame->lights)
	{
		if (l.radius > 0.0f)
			level += Importance(s, PointLight(s, l, 1.0f));
		if (sc.fog_density > 0.0f)
		{
			const Vec3 d = l.origin - eye;
			const float t0 = Dot(d, dir);
			const float h = std::max(std::sqrt(std::max(Dot(d, d) - t0 * t0, 0.0f)), 1.0f);
			level += Luminance(l.emission) * sc.fog_density * (0.25f * kInvPi)
				* (std::atan((reach - t0) / h) + std::atan(t0 / h)) / h;
		}
	}
	return level;
}

// keeps a sampled direction on the outside of the real surface
Vec3 AboveSurface(const Surface &s, Vec3 wi)
{
	const float below = Dot(wi, s.ng);
	return below < 0.0f ? wi - s.ng * (2.0f * below) : wi;
}

Vec3 SampleDiffuse(const Surface &s, Rng &rng)
{
	const float r1 = rng.Float(), r2 = rng.Float();
	const float r = std::sqrt(r1), phi = 2.0f * kPi * r2;
	Vec3 t, b;
	Basis(s.n, t, b);
	return AboveSurface(s, t * (r * std::cos(phi)) + b * (r * std::sin(phi)) + s.n * std::sqrt(std::max(0.0f, 1.0f - r1)));
}

// Samples the normals visible from wo (Heitz, "Sampling the GGX Distribution
// of Visible Normals", JCGT 2018) and reflects wo about the one drawn
bool SampleSpecular(const Surface &s, Rng &rng, Vec3 &wi, Vec3 &weight)
{
	Vec3 t, b;
	Basis(s.n, t, b);
	const Vec3 ve(Dot(s.wo, t), Dot(s.wo, b), Dot(s.wo, s.n));
	if (ve.z <= 0.0f)
		return false;

	const float a = s.alpha;
	const Vec3 vh = Normalize(Vec3(a * ve.x, a * ve.y, ve.z));
	const float lensq = vh.x * vh.x + vh.y * vh.y;
	const Vec3 t1 = lensq > 0.0f ? Vec3(-vh.y, vh.x, 0.0f) / std::sqrt(lensq) : Vec3(1, 0, 0);
	const Vec3 t2 = Cross(vh, t1);

	const float u1 = rng.Float(), u2 = rng.Float();
	const float r = std::sqrt(u1), phi = 2.0f * kPi * u2;
	const float p1 = r * std::cos(phi);
	float p2 = r * std::sin(phi);
	const float sblend = 0.5f * (1.0f + vh.z);
	p2 = (1.0f - sblend) * std::sqrt(std::max(0.0f, 1.0f - p1 * p1)) + sblend * p2;
	const Vec3 nh = t1 * p1 + t2 * p2 + vh * std::sqrt(std::max(0.0f, 1.0f - p1 * p1 - p2 * p2));
	const Vec3 hl = Normalize(Vec3(a * nh.x, a * nh.y, std::max(0.0f, nh.z)));
	const Vec3 h = t * hl.x + b * hl.y + s.n * hl.z;

	const float voh = Dot(s.wo, h);
	wi = h * (2.0f * voh) - s.wo;
	const float nol = Dot(s.n, wi), nov = ve.z;
	if (nol <= 0.0f || voh <= 0.0f)
		return false;
	wi = AboveSurface(s, wi);

	// value * cos / pdf comes down to F * G2 / G1(wo)
	const float lv = SmithLambda(a, nov), ll = SmithLambda(a, nol);
	const float g2_over_g1 = nol * (nov + lv) / (nov * ll + nol * lv);
	weight = Fresnel(s.f0, voh) * g2_over_g1;
	return true;
}

Vec3 Radiance(const Scene &sc, Ray ray, Rng &rng, bool camera, bool count_emitters, int depth, int max_bounces,
	float *reached, int *followed)
{
	Vec3 radiance, throughput(1, 1, 1);
	if (reached)
		*reached = FLT_MAX;
	// in the white furnace every path ends in the same light
	const bool furnace = sc.view_mode == PT_VIEW_FURNACE;

	for (;; depth++, camera = false)
	{
		Hit hit;
		const Tri *tri;
		if (followed)
			++*followed;
		if (!Closest(sc, ray, rng, camera, true, hit, tri))
			return furnace ? radiance + throughput * PT_FURNACE_LIGHT : radiance;
		if (reached)
		{
			*reached = hit.t;
			reached = nullptr;
		}
		if (tri->mat->flags & PT_MAT_SKY)
		{
			if (furnace)
				return radiance + throughput * PT_FURNACE_LIGHT;
			return (camera || count_emitters || sc.world->sky_cdf.empty())
				? radiance + throughput * sc.Sky(ray.d) : radiance;
		}

		Surface s;
		MakeSurface(sc, *tri, hit, ray, s);
		const Material &mat = *s.mat;

		if (mat.emissive && s.front && (camera || count_emitters || !mat.sampled))
			radiance += throughput * Emitted(s, camera);

		const Vec3 ks = s.SpecularAlbedo();
		const float ld = Luminance(s.kd), ls = Luminance(ks);
		if (ld + ls <= 0.0f)
			return radiance;		// reflects nothing

		// The shine takes its share of the light first and the matte part
		// has what is left, so the two together never reflect more than
		// falls on them. The share is that of one way the light could be
		// mirrored, drawn here and followed below if the path goes that way.
		Vec3 mirrored, shine;
		if (ls <= 0.0f || !SampleSpecular(s, rng, mirrored, shine))
			shine = Vec3();
		const Vec3 matte = s.kd * (Vec3(1, 1, 1) - shine);

		const Lit world = DirectWorld(sc, s, rng, false), frame = DirectFrameOne(sc, s, rng);
		radiance += throughput * (matte * (world.diffuse + frame.diffuse) * kInvPi + world.specular + frame.specular);

		if (depth >= max_bounces)
			return furnace ? radiance + throughput * PT_FURNACE_LIGHT : radiance;

		Vec3 wi;
		if (sc.reflections < 2)
		{
			// Shiny surfaces still show highlights from lights, but nothing
			// is followed off them: the matte part stands in for the shine,
			// and carries its share too.
			if (ld <= 0.0f)
				return radiance;
			wi = SampleDiffuse(s, rng);
			throughput *= s.kd;
			count_emitters = false;
		}
		else
		{
			// continue through one lobe, chosen by how much each reflects
			const float lm = Luminance(matte), lh = Luminance(shine);
			if (lm + lh <= 0.0f)
				return radiance;
			const float pick_spec = lh / (lm + lh);
			if (rng.Float() < pick_spec)
			{
				wi = mirrored;
				throughput *= shine * (1.0f / pick_spec);
				count_emitters = !s.light_sampled_spec;
			}
			else
			{
				wi = SampleDiffuse(s, rng);
				throughput *= matte * (1.0f / (1.0f - pick_spec));
				count_emitters = false;
			}
		}

		// paths that carry little are ended at random, the rest made to count for them
		if (depth >= 1)
		{
			const float survive = std::min(1.0f, std::max(0.1f, MaxComponent(throughput)));
			if (rng.Float() >= survive)
				return radiance;
			throughput *= 1.0f / survive;
		}

		ray.o = s.p + s.ng * kRayOffset;
		ray.d = wi;
		ray.tmin = 0.0f;
		ray.tmax = FLT_MAX;
	}
}

} // namespace PT_NS
