// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Jonathan Ferguson
//
// CPU backend: a unidirectional path tracer with next event estimation,
// temporal accumulation and an edge aware filter, presented through GDI.

#include "../include/pt.h"
#include "pt_pool.h"
#include "pt_trace.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#endif
#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
#include <intrin.h>
#elif defined(__GNUC__) && (defined(__x86_64__) || defined(__i386__))
#include <cpuid.h>
#endif

#include <algorithm>
#include <cfloat>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>

namespace {

using namespace PT_NS;

const int kMaxFilterPasses = 4;

// With automatic exposure the picture's typical luminance is brought to this
// before the user's exposure is applied.
const float kTypicalTarget = 0.0054f;
const float kMinDemodulate = 0.02f;		// reflectance floor when lighting is divided by it

// kOver: light from see-through layers in front of the surface, which has no
// reflectance of its own to be multiplied by
// kFog: light scattered towards the eye by the air in front of the surface
enum { kDiffuse, kSpecular, kOver, kFog, kChannels };

const int kNumStages = 5;		// the parts a view's time is counted in

uint8_t g_to_display[4097];

void InitTables()
{
	static bool done = false;
	if (done)
		return;
	done = true;
	InitColourTables();
	for (int i = 0; i <= 4096; i++)
		g_to_display[i] = (uint8_t)(std::pow(i / 4096.0f, 1.0f / 2.2f) * 255.0f + 0.5f);
}

struct Camera
{
	Vec3	origin, forward, right, up;
	float	tx = 0, ty = 0;		// tangents of the half angles

	bool operator==(const Camera &o) const { return !memcmp(this, &o, sizeof(*this)); }
};

/*
Per pixel state at render resolution.

Lighting is kept apart from the reflectance of the surface it falls on, and
split by the lobe that reflects it: light[kDiffuse] is multiplied by
albedo[kDiffuse], light[kSpecular] by albedo[kSpecular], with `add` on top
for what needs no filtering (what the surface emits, the frame's point
lights). That lets the noisy part be averaged over time and space without
smearing textures.
*/
struct Pixels
{
	std::vector<Vec3>	pos;
	std::vector<Vec3>	seen;		// where it appears to be: along the eye's ray, even if water bent the view
	std::vector<uint8_t> bent;		// seen through refraction
	std::vector<uint8_t> moved;		// on something that was elsewhere last frame; seen is where
	std::vector<Vec3>	spec_pos;	// where a mirror-like surface's reflection appears to be
	std::vector<uint8_t> spec_ok;	// spec_pos is set
	std::vector<Vec3>	plane;		// geometric normal
	std::vector<Vec3>	normal;		// shading normal
	std::vector<float>	depth;		// along the ray; negative where there is no surface
	std::vector<float>	roughness;
	std::vector<Vec3>	add;
	std::vector<Vec3>	albedo[kChannels];
	std::vector<Vec3>	light[kChannels];		// this frame's samples, then accumulated
	std::vector<float>	m1[kChannels], m2[kChannels];	// luminance moments of light
	std::vector<float>	variance[kChannels];
	std::vector<float>	length;		// frames accumulated
	std::vector<float>	flash;		// what the frame's lights put on the pixel, see FrameLightLevel
	std::vector<Vec3>	over_pos;	// where what kOver shows appears to be: a reflection sits behind the glass
	std::vector<float>	over_length;	// frames accumulated in kOver, which is followed separately

	void Resize(size_t n)
	{
		pos.assign(n, Vec3());
		seen.assign(n, Vec3());
		bent.assign(n, 0);
		moved.assign(n, 0);
		spec_pos.assign(n, Vec3());
		spec_ok.assign(n, 0);
		plane.assign(n, Vec3());
		normal.assign(n, Vec3());
		depth.assign(n, -1.0f);
		roughness.assign(n, 1.0f);
		add.assign(n, Vec3());
		length.assign(n, 0.0f);
		flash.assign(n, 0.0f);
		over_pos.assign(n, Vec3());
		over_length.assign(n, 0.0f);
		for (int c = 0; c < kChannels; c++)
		{
			albedo[c].assign(n, Vec3());
			light[c].assign(n, Vec3());
			m1[c].assign(n, 0.0f);
			m2[c].assign(n, 0.0f);
			variance[c].assign(n, 0.0f);
		}
	}
};

#ifdef PT_AVX2_KERNELS

// What the filter reads. The AVX2 build filters eight pixels of a row at a
// time, so it keeps a row of the picture as a row of each quantity in turn:
// eight neighbours are then eight numbers side by side.
enum
{
	kGeoPosX, kGeoPosY, kGeoPosZ, kGeoDepth, kGeoNormalX, kGeoNormalY, kGeoNormalZ, kGeoRoughness,
	kGeoPlaneX, kGeoPlaneY, kGeoPlaneZ, kGeoRows
};
// red, green, blue and variance of each channel, and one row unused: with an
// odd number the rows of the picture do not all fall in the same cache sets
enum { kLightRows = 4 * kChannels + 1 };

struct FilterRows
{
	// either side of every row, so that eight neighbours of which some are
	// off the picture can still be read, and eight pixels of which some are
	// past the end of the row still written
	enum { kMargin = 8 };

	std::vector<float>	data;
	size_t	stride = 0;		// from the row of one quantity to that of the next
	int		quantities = 0;

	void Resize(int rw, int rh, int count)
	{
		quantities = count;
		stride = ((size_t)rw + 2 * kMargin + 15) / 16;
		stride = (stride | 1) * 16;		// an odd number of cache lines, for the same reason
		data.assign(stride * quantities * rh, 0.0f);
	}

	// row y of the picture, at its first quantity and first pixel
	float *Row(int y) { return data.data() + (size_t)y * quantities * stride + kMargin; }
	const float *Row(int y) const { return data.data() + (size_t)y * quantities * stride + kMargin; }
};

#else

// What the filter reads, packed so that a neighbour is two cache lines
// rather than a dozen
struct FilterGeo
{
	Vec3	pos;
	float	depth;
	Vec3	normal;
	float	roughness;
	Vec3	plane;
	float	pad;
};

// one lane per channel, the fourth unused
struct FilterLight
{
	float	r[4], g[4], b[4];
	float	var[4];
};

#endif

struct CpuBackend
{
	pt_backend_t	base{};
	pt_log_fn		log = nullptr;
#ifdef _WIN32
	HWND			hwnd = nullptr;
	HDC				memdc = nullptr;
	HBITMAP			dib = nullptr;
	HGDIOBJ			olddib = nullptr;
#else
	Display			*display = nullptr;
	Window			window = 0;
	GC				gc = nullptr;
	XImage			*image = nullptr;	// its data is dibbits
#endif
	uint32_t		*dibbits = nullptr;		// 0x00RRGGBB, top row first
	int				width = 0, height = 0;
	std::vector<uint32_t> scene;			// 0x00RRGGBB, window sized
	bool			has_view = false;
	int				shown[4] = {0, 0, 0, 0};	// the part of the window the last view covered
	pt_view_t		view{};

	Pool					pool;
	std::unique_ptr<World>	world;
	std::vector<std::unique_ptr<Texture>> textures;	// by handle
	Frame					frame;

	Pixels					cur, prev;
#ifdef PT_AVX2_KERNELS
	FilterRows				filter_geo, filter_a, filter_b;
#else
	std::vector<FilterGeo>	filter_geo;
	std::vector<FilterLight> filter_a, filter_b;
#endif
	std::vector<Vec3>		hdr;			// this frame's picture, at traced resolution
	std::vector<Vec3>		near_lo, near_hi;	// range of hdr around each traced pixel
	std::vector<Vec3>		bloom_a, bloom_b;
	// the picture blended over time, at the view's resolution
	std::vector<Vec3>		steady, steady_prev;
	std::vector<float>		steady_count, steady_count_prev;	// how much is in each pixel's blend
	int						out_w = 0, out_h = 0;
	float					auto_exposure = 1.0f;
	float					prev_time = 0.0f;
	std::vector<uint32_t>	ldr;			// tone mapped, render sized
	int						rw = 0, rh = 0;
	bool					have_history = false;
	bool					have_exposure = false;	// auto_exposure has been measured at least once
	bool					antialiased = false;	// last frame was
	float					jitter_x = 0.0f, jitter_y = 0.0f;	// this frame's offset within the pixel
	int						filtering = 2;		// what is done about noise this frame, see pt_view_t
	float					moving_history = 32.0f;	// frames of lighting kept while anything changes
	float					fog_history = 32.0f;	// and of the air's light, which has nothing to be followed by
	bool					reflection_history = true;	// reflections are followed where they appear to be
	Camera					prev_camera;
	uint32_t				prev_hash = 0;
	uint32_t				frame_index = 0;
	char					stats[160] = "";
	float					stage_ms[kNumStages] = {};	// where the last view's time went
	bool					stages_new = false;			// and nobody has asked since
	char					device[96] = "";
};

const char *const kStageNames[kNumStages] = {"build", "trace", "history", "filter", "out"};

CpuBackend *Self(pt_backend_t *b) { return reinterpret_cast<CpuBackend *>(b); }

// the processor as it names itself, and how many threads trace
void NameDevice(char *out, size_t size, int threads)
{
	char brand[49] = "";
#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
	int regs[4];
	__cpuid(regs, 0x80000000);
	if ((unsigned)regs[0] >= 0x80000004u)
		for (int i = 0; i < 3; i++)
		{
			__cpuid(regs, 0x80000002 + i);
			memcpy(brand + i * 16, regs, 16);
		}
#elif defined(__GNUC__) && (defined(__x86_64__) || defined(__i386__))
	unsigned int regs[4];
	if (__get_cpuid_max(0x80000000, nullptr) >= 0x80000004u)
		for (int i = 0; i < 3; i++)
		{
			__get_cpuid(0x80000002 + i, &regs[0], &regs[1], &regs[2], &regs[3]);
			memcpy(brand + i * 16, regs, 16);
		}
#endif
	// it comes padded with spaces
	const char *first = brand;
	while (*first == ' ')
		first++;
	size_t len = strlen(first);
	while (len && first[len - 1] == ' ')
		len--;
	// and which of the two builds of this backend is the one running
#ifdef PT_AVX2_KERNELS
	const char *const built_for = "AVX2";
#else
	const char *const built_for = "SSE";
#endif
	snprintf(out, size, "%.*s%s%d threads, %s", (int)len, first, len ? ", " : "", threads, built_for);
}

void Destroy(pt_backend_t *b)
{
	CpuBackend *s = Self(b);
#ifdef _WIN32
	if (s->memdc)
	{
		if (s->olddib)
			SelectObject(s->memdc, s->olddib);
		DeleteDC(s->memdc);
	}
	if (s->dib)
		DeleteObject(s->dib);
#else
	if (s->image)
	{
		s->image->data = nullptr;	// ours, not Xlib's to free
		XDestroyImage(s->image);
	}
	if (s->gc)
		XFreeGC(s->display, s->gc);
	free(s->dibbits);
#endif
	delete s;
}

void LoadWorld(pt_backend_t *b, const pt_world_t *world)
{
	CpuBackend *s = Self(b);
	s->world.reset();
	s->have_history = false;
	if (!world)
		return;
	s->world = BuildWorld(world);

	if (s->log)
	{
		size_t emitters = 0;
		float tripower = 0, pointpower = 0;
		for (const Light &l : s->world->lights)
		{
			if (l.tri != ~0u) { emitters++; tripower += l.pdf; }
			else pointpower += l.pdf;
		}
		char msg[160];
		snprintf(msg, sizeof(msg), "CPU path tracer: %zu emitting triangles (%.0f%% of power), %zu point lights (%.0f%%)\n",
			emitters, tripower * 100.0f, s->world->lights.size() - emitters, pointpower * 100.0f);
		s->log(msg);
	}
}

int TextureCreate(pt_backend_t *b, const pt_texture_t *texture)
{
	CpuBackend *s = Self(b);
	if (!texture || texture->width <= 0 || texture->height <= 0 || !texture->pixels)
		return -1;

	size_t slot = 0;
	while (slot < s->textures.size() && s->textures[slot])
		slot++;
	if (slot == s->textures.size())
		s->textures.emplace_back();
	s->textures[slot].reset(new Texture);
	s->textures[slot]->Set(*texture);
	return (int)slot;
}

void TextureUpdate(pt_backend_t *b, int handle, const uint32_t *pixels)
{
	CpuBackend *s = Self(b);
	if (handle < 0 || handle >= (int)s->textures.size() || !s->textures[handle] || !pixels)
		return;
	Texture &t = *s->textures[handle];
	t.pixels.assign(pixels, pixels + (size_t)t.width * t.height);
}

void TextureDestroy(pt_backend_t *b, int handle)
{
	CpuBackend *s = Self(b);
	if (handle >= 0 && handle < (int)s->textures.size())
		s->textures[handle].reset();
}

void ClipView(const CpuBackend *s, int &x0, int &y0, int &x1, int &y1)
{
	x0 = s->view.x < 0 ? 0 : s->view.x;
	y0 = s->view.y < 0 ? 0 : s->view.y;
	x1 = s->view.x + s->view.width;
	y1 = s->view.y + s->view.height;
	if (x1 > s->width) x1 = s->width;
	if (y1 > s->height) y1 = s->height;
}

// How the finished picture is graded for the screen
struct Grade
{
	int		curve = 0;			// 0 filmic, 1 neutral, 2 clipped like the original
	float	saturation = 1.0f;
	float	contrast = 1.0f;
};

uint32_t ToneMap(Vec3 c, const Grade &g)
{
	float v[3];
	for (int i = 0; i < 3; i++)
	{
		const float x = c[i] > 0.0f ? c[i] : 0.0f;
		float y;
		switch (g.curve)
		{
		case 1:		// rolls off gently towards white at 4
			y = x * (1.0f + x * (1.0f / 16.0f)) / (1.0f + x);
			break;
		case 2:
			y = x;
			break;
		default:	// Narkowicz's fit to the ACES filmic curve
			y = (x * (2.51f * x + 0.03f)) / (x * (2.43f * x + 0.59f) + 0.14f);
			break;
		}
		v[i] = y < 0.0f ? 0.0f : (y > 1.0f ? 1.0f : y);
	}

	if (g.saturation != 1.0f || g.contrast != 1.0f)
	{
		const float lum = 0.2126f * v[0] + 0.7152f * v[1] + 0.0722f * v[2];
		for (int i = 0; i < 3; i++)
		{
			float y = lum + (v[i] - lum) * g.saturation;
			if (y < 0.0f) y = 0.0f;
			if (g.contrast != 1.0f)
				y = 0.18f * std::pow(y * (1.0f / 0.18f), g.contrast);	// pivots on mid grey
			v[i] = y > 1.0f ? 1.0f : y;
		}
	}

	return ((uint32_t)g_to_display[(int)(v[0] * 4096.0f)] << 16)
		| ((uint32_t)g_to_display[(int)(v[1] * 4096.0f)] << 8)
		| (uint32_t)g_to_display[(int)(v[2] * 4096.0f)];
}

Vec3 Demodulate(Vec3 light, Vec3 reflectance)
{
	return Vec3(light.x / reflectance.x, light.y / reflectance.y, light.z / reflectance.z);
}

// Light meeting the boundary between two clear materials: how much is
// reflected, and the cosine of the angle the rest leaves at. eta is the
// refractive index on the side the light comes from over that on the other.
// Returns 1 when nothing gets through (total internal reflection).
float Dielectric(float cosi, float eta, float &cost)
{
	const float sin2t = eta * eta * (1.0f - cosi * cosi);
	if (sin2t >= 1.0f)
	{
		cost = 0.0f;
		return 1.0f;
	}
	cost = std::sqrt(1.0f - sin2t);
	const float rs = (eta * cosi - cost) / (eta * cosi + cost);
	const float rp = (cosi - eta * cost) / (cosi + eta * cost);
	return 0.5f * (rs * rs + rp * rp);
}

const float kGlassIndex = 1.5f;

// what is left of light after a distance through something that absorbs it
Vec3 Fade(Vec3 absorb, float distance)
{
	if (distance <= 0.0f || MaxComponent(absorb) <= 0.0f)
		return Vec3(1, 1, 1);
	return Vec3(std::exp(-absorb.x * distance), std::exp(-absorb.y * distance), std::exp(-absorb.z * distance));
}

// the ray from the eye through a point of the picture
Ray EyeRay(const CpuBackend *s, const Camera &cam, float px, float py)
{
	Ray ray;
	ray.o = cam.origin;
	ray.d = Normalize(cam.forward
		+ cam.right * ((2.0f * px / s->rw - 1.0f) * cam.tx)
		+ cam.up * ((1.0f - 2.0f * py / s->rh) * cam.ty));
	ray.tmin = 0.0f;
	ray.tmax = FLT_MAX;
	return ray;
}

// Traces one pixel: what the eye sees there, and samples of the light on it.
// frame_cam is the eye when the shutter closes; with motion blur the pixel
// sees from where the eye was at a moment of its own before that.
// The light the air scatters to the eye from the first reach of a view ray:
// the reach is cut into as many lengths as there are samples, and a point
// drawn in each.
Vec3 FogGlow(const Scene &sc, Vec3 from, Vec3 dir, float reach, Rng &rng)
{
	const int count = std::max(1, sc.fog_samples);
	Vec3 sum;
	for (int k = 0; k < count; k++)
	{
		const float at = (k + rng.Float()) * reach / count;
		const Vec3 lit = DirectMedium(sc, from + dir * at, rng);
		sum += ClampSample(lit * (sc.fog_density * (0.25f * kInvPi) * std::exp(-sc.fog_density * at) * reach), sc.max_sample);
	}
	return sum * (1.0f / count);
}

void TracePixel(CpuBackend *s, const Scene &sc, const Camera &frame_cam, float jx, float jy, int x, int y, int samples, int bounces)
{
	const size_t i = (size_t)y * s->rw + x;
	Pixels &px = s->cur;
	Rng rng(Hash((uint32_t)i, s->frame_index));

	Camera cam = frame_cam;
	if (s->view.blur)
	{
		// between the shutter opening and closing, in a straight line, the
		// axes squared up again afterwards
		const float t = rng.Float();
		const Vec3 open_origin(s->view.open_origin), open_forward(s->view.open_forward), open_right(s->view.open_right);
		cam.origin = open_origin + (frame_cam.origin - open_origin) * t;
		cam.forward = Normalize(open_forward + (frame_cam.forward - open_forward) * t);
		const Vec3 right = open_right + (frame_cam.right - open_right) * t;
		cam.right = Normalize(right - cam.forward * Dot(right, cam.forward));
		cam.up = Cross(cam.right, cam.forward);
	}

	Ray ray = EyeRay(s, cam, x + 0.5f + jx, y + 0.5f + jy);
	Vec3 eye_dir = ray.d;

	// What the eye carries, the weapon in hand, turns with it: it is seen
	// along the unblurred ray, from the eye at the end of the frame, and the
	// blurred ray passes through where it is.
	const bool held_apart = s->view.blur && sc.frame->has_held;
	Ray held_ray;
	Hit held_hit;
	const Tri *held_tri = nullptr;
	if (held_apart)
	{
		held_ray = EyeRay(s, frame_cam, x + 0.5f + jx, y + 0.5f + jy);
		if (!Closest(sc, held_ray, rng, true, false, held_hit, held_tri, kHeldOnly))
			held_tri = nullptr;
	}

	px.depth[i] = -1.0f;
	px.add[i] = Vec3();
	px.flash[i] = 0.0f;
	px.over_pos[i] = Vec3();
	px.bent[i] = 0;
	px.moved[i] = 0;
	px.spec_ok[i] = 0;
	for (int c = 0; c < kChannels; c++)
	{
		px.albedo[c][i] = Vec3();
		px.light[c][i] = Vec3();
		px.m1[c][i] = px.m2[c][i] = 0.0f;
	}

	// Walk through whatever is see-through to the first solid surface. The
	// solid surface is what the filters work on; the layers in front dim it
	// and add their own light on top.
	float through = 1.0f;		// how much of what is behind still shows
	Vec3 tint(1, 1, 1);			// what liquid on the way has left of each colour
	Vec3 absorb(s->view.medium_absorb);	// of the liquid the path is in now; none in air
	Vec3 shaft;					// what that liquid has scattered towards the eye
	bool shafted = false;		// whether any of the path was in one that scatters
	// the view modes that leave part of the light out, or measure something
	const int view_mode = sc.view_mode;
	const bool furnace = view_mode == PT_VIEW_FURNACE;
	const float direct_on = view_mode == PT_VIEW_INDIRECT ? 0.0f : 1.0f;
	const float indirect_on = view_mode == PT_VIEW_DIRECT ? 0.0f : 1.0f;
	if (furnace)
		absorb = Vec3();
	float entered = 0.0f;		// where along the current ray that began
	float travelled = 0.0f;		// along the path so far, which water may have bent
	Vec3 front_add;				// from the layers: what they emit
	Vec3 front_diffuse;			// what they scatter, which is noisy
	Vec3 front_mirror;			// and what they mirror, noisy too
	float mirror_weight = 0.0f;	// of the strongest reflection so far
	bool have_layer = false;	// first_layer is set
	Surface first_layer;
	float first_layer_depth = 0.0f;
	bool solid = false;
	Hit hit;
	const Tri *tri;
	Surface surf;
	Vec3 sky;
	bool sky_hit = false;		// the view ends at the sky, at sky_p
	Vec3 sky_p, sky_n;
	float sky_depth = 0.0f;

	for (int layer = 0; through > 0.001f; layer++)
	{
		bool found = Closest(sc, ray, rng, true, false, hit, tri, held_apart ? kNotHeld : kHeldToo, true);
		if (held_tri && travelled <= 0.0f && (!found || held_hit.t < hit.t))
		{
			// the weapon is the nearest thing: from here on this is the
			// unblurred path
			hit = held_hit;
			tri = held_tri;
			ray = held_ray;
			held_tri = nullptr;
			cam = frame_cam;
			eye_dir = ray.d;
			found = true;
		}
		if (!found)
		{
			if (furnace)
				sky = Vec3(PT_FURNACE_LIGHT * through);
			break;
		}
		if (tri->mat->flags & PT_MAT_SKY)
		{
			sky = sc.Sky(ray.d) * through;
			if (view_mode)
				sky = furnace ? Vec3(PT_FURNACE_LIGHT * through)
					: (view_mode >= PT_VIEW_BASE_COLOUR ? Vec3() : sky * direct_on);
			sky_hit = true;
			sky_p = ray.o + ray.d * hit.t;
			sky_n = Dot(tri->n, ray.d) < 0.0f ? tri->n : -tri->n;
			sky_depth = travelled + hit.t;
			break;
		}
		MakeSurface(sc, *tri, hit, ray, surf, sc.filter_textures, !view_mode);
		const Material &mat = *surf.mat;
		// A liquid scatters a little of the light in it towards the eye: it
		// glows where light comes down through it, in shafts the waves and
		// whatever stands in the light's way break up.
		const float shafts = sc.water_shafts * (absorb.x + absorb.y + absorb.z) * 0.1f;	// what it scatters, per unit: a third of what it soaks up
		// (looked for one frame in four, and counted four times: it is faint, and gathered)
		if (shafts > 0.0f && !view_mode && hit.t > entered)
		{
			if (rng.Float() < 0.25f)
			{
				const float at = entered + (hit.t - entered) * rng.Float();
				shaft += ClampSample(DirectMedium(sc, ray.o + ray.d * at, rng) * (shafts * kInvPi * (hit.t - entered)), sc.max_sample)
					* (tint * Fade(absorb, at - entered) * through);
			}
			shafted = true;
		}
		tint *= Fade(absorb, hit.t - entered);
		entered = hit.t;
		if (view_mode >= PT_VIEW_LIGHTING)
		{
			if (view_mode == PT_VIEW_LIGHTING)
				WhiteSurface(surf);
			else if (view_mode >= PT_VIEW_BASE_COLOUR && view_mode <= PT_VIEW_GLOW)
			{
				// the first surface met is all there is to show
				px.add[i] = SurfaceChannel(view_mode, surf);
				px.pos[i] = surf.p;
				px.seen[i] = surf.p;
				px.plane[i] = surf.ng;
				px.normal[i] = surf.n;
				px.depth[i] = hit.t;
				px.roughness[i] = surf.roughness;
				return;
			}
		}
		if (mat.alpha >= 1.0f || layer >= 8 || (view_mode && ViewSolid(view_mode, mat)))
		{
			solid = true;
			break;
		}

		if (!have_layer)
		{
			have_layer = true;
			first_layer = surf;
			first_layer_depth = travelled + hit.t;
		}
		px.over_pos[i] = mirror_weight > 0.0f ? px.over_pos[i] : surf.p;

		// Glass and water are clear materials with a tint painted on. The
		// boundary reflects some of the light, more at a glancing angle, and
		// lets the rest in; water bends it on the way.
		bool bent = false, shut = false;
		if (!(mat.flags & PT_MAT_BLACK) && surf.roughness < kLightSampledRoughness)
		{
			const bool liquid = (mat.flags & PT_MAT_WAVES) != 0 && sc.refraction;
			const float cosi = std::min(1.0f, std::max(0.0f, Dot(surf.n, surf.wo)));
			float fresnel, cost = 0.0f;
			float eta = 1.0f;
			if (liquid)
			{
				// its underside faces down: there the eye is in the water looking out
				eta = tri->n.z < -0.5f ? Scene::kWaterIndex : 1.0f / Scene::kWaterIndex;
				fresnel = Dielectric(cosi, eta, cost);
			}
			else
			{
				// a pane: in one face and out the other with no net bend, and
				// both faces reflect
				const float one = Dielectric(cosi, 1.0f / kGlassIndex, cost);
				fresnel = 2.0f * one / (1.0f + one);
			}

			// with reflections off the light simply all goes through
			if (sc.reflections < 1 && fresnel < 1.0f)
				fresnel = 0.0f;
			// froth is wet and shines, but less than the liquid under it
			const float shine = fresnel * (1.0f - 0.75f * surf.foam);
			shut = fresnel >= 1.0f;
			// Froth is matt, and lit by the room as well as by its lamps.
			// Half the rays that would have gone to look for what the
			// surface mirrors go to look for that light instead; from under
			// the liquid it is the light above that comes through.
			const bool scatter = surf.foam > 0.0f && rng.Float() < 0.5f;
			const float chosen = surf.foam > 0.0f ? 0.5f : 1.0f;
			if (shine > 0.0f || scatter)
			{
				Ray mirror;
				float side = 1.0f;
				mirror.d = surf.n * (2.0f * Dot(surf.n, surf.wo)) - surf.wo;
				if (Dot(mirror.d, surf.ng) < 0.0f)
					mirror.d = mirror.d - surf.ng * (2.0f * Dot(mirror.d, surf.ng));
				if (scatter)
				{
					side = tri->n.z < -0.5f ? -1.0f : 1.0f;
					mirror.d = SampleDiffuse(surf, rng) * side;
				}
				mirror.o = surf.p + surf.ng * (side * kRayOffset);
				mirror.tmin = 0.0f;
				mirror.tmax = FLT_MAX;

				float reached = 0.0f;
				const float weight = scatter ? 0.0f : through * shine;
				const Vec3 found = ClampSample(Radiance(sc, mirror, rng, false, !scatter, 1, sc.reflection_bounces, &reached), sc.max_sample);
				if (scatter)
					front_mirror += found * surf.kd * (through * (1.0f - shine) * (shut ? 1.0f : surf.cover) / chosen);
				else
					front_mirror += found * (weight / chosen);

				// A reflection appears to sit behind the glass, as far again
				// as the thing reflected is in front. That is where to look
				// for it in last frame's picture.
				if (weight > mirror_weight)
				{
					mirror_weight = weight;
					px.over_pos[i] = cam.origin + eye_dir * (travelled + hit.t + std::min(reached, 100000.0f));
				}
			}
			through *= 1.0f - shine;

			// going in, the liquid starts soaking up light; coming out, it stops
			if (mat.flags & PT_MAT_WAVES)
				absorb = tri->n.z < -0.5f ? Vec3() : mat.absorb;

			if (liquid && fresnel < 1.0f)
			{
				// into (or out of) the water
				travelled += hit.t;
				ray.o = surf.p - surf.ng * kRayOffset;
				ray.d = Normalize(ray.d * eta + surf.n * (eta * cosi - cost));
				if (Dot(ray.d, surf.ng) > 0.0f)
					ray.d = Normalize(ray.d - surf.ng * (2.0f * Dot(ray.d, surf.ng)));
				ray.tmin = 0.0f;
				entered = 0.0f;
				bent = true;
			}
		}

		// the painted tint: covers its share of what is behind and shows lit itself
		// where the liquid lets nothing out, froth has all that it does not mirror
		const float cover = shut && surf.foam > 0.0f ? 1.0f : surf.cover;
		const float share = through * cover;
		if (surf.foam > 0.0f && tri->n.z < -0.5f)
		{
			// froth seen from under the liquid: what lights it is above,
			// and comes through it
			surf.n = -surf.n;
			surf.ng = -surf.ng;
		}
		if (mat.emissive && surf.front)
			front_add += Emitted(surf, true) * share;
		if (share > 0.0f && MaxComponent(surf.kd) > 0.0f)
		{
			const Lit world = DirectWorld(sc, surf, rng, true), frame = DirectFrameOne(sc, surf, rng);
			front_diffuse += surf.kd * (world.diffuse + frame.diffuse) * (kInvPi * share);
		}
		through *= 1.0f - cover;
		if (!bent)
			ray.tmin = hit.t + 0.01f;
	}

	// the layers' light is noisy and gets a channel of its own
	const Vec3 over = ClampSample(front_diffuse * direct_on + front_mirror * indirect_on, sc.max_sample);
	const float over_lum = Luminance(over);
	// whether there is a layer must not depend on what this frame's sample
	// happened to find, or its history would start over at random
	if (have_layer)
	{
		px.albedo[kOver][i] = Vec3(1, 1, 1);
		px.light[kOver][i] = over;
		px.m1[kOver][i] = over_lum;
		px.m2[kOver][i] = over_lum * over_lum;
	}

	if (!solid)
	{
		// Nothing solid behind: sky, or nothing at all. If a layer was
		// crossed it stands in as the surface, so that its light is still
		// averaged over time; failing that, the place where the sky begins.
		px.add[i] = front_add * direct_on + sky * tint;
		if (have_layer)
		{
			px.pos[i] = first_layer.p;
			px.seen[i] = first_layer.p;
			px.plane[i] = first_layer.ng;
			px.normal[i] = first_layer.n;
			px.depth[i] = first_layer_depth;
			px.roughness[i] = first_layer.roughness;
		}
		else if (sky_hit)
		{
			px.pos[i] = sky_p;
			px.seen[i] = sky_p;
			px.plane[i] = sky_n;
			px.normal[i] = sky_n;
			px.depth[i] = sky_depth;
			px.roughness[i] = 1.0f;
		}

		// the air in front of the sky scatters light like any other: the
		// sky is dimmed by it and the shafts in it show
		if (sc.fog_density > 0.0f && px.depth[i] >= 0.0f)
		{
			const float reach = have_layer ? first_layer_depth : sky_depth;
			const Vec3 glow = FogGlow(sc, cam.origin, eye_dir, reach, rng) * direct_on;
			const float glow_lum = Luminance(glow);
			px.albedo[kFog][i] = Vec3(1, 1, 1);
			px.light[kFog][i] = glow;
			px.m1[kFog][i] = glow_lum;
			px.m2[kFog][i] = glow_lum * glow_lum;

			const float kept = std::exp(-sc.fog_density * (sky_hit ? sky_depth : first_layer_depth));
			px.albedo[kOver][i] *= kept;
			px.add[i] *= kept;
		}
		if (shafted)
		{
			// with the air's: it is the same kind of light
			px.albedo[kFog][i] = Vec3(1, 1, 1);
			px.light[kFog][i] += shaft * direct_on;
			px.m1[kFog][i] = Luminance(px.light[kFog][i]);
			px.m2[kFog][i] = px.m1[kFog][i] * px.m1[kFog][i];
		}
		if (view_mode == PT_VIEW_COST)
			px.light[kDiffuse][i] = Vec3((float)rng.rays);
		return;
	}
	if (!view_mode)
		AddWater(sc, surf, MaxComponent(absorb) > 0.0f);

	const Material &mat = *surf.mat;
	const Vec3 kd = surf.kd * through;
	const Vec3 spec_albedo = Max(surf.SpecularAlbedo(), Vec3(kMinDemodulate));
	const Vec3 ks = spec_albedo * through;
	const bool has_diffuse = MaxComponent(surf.kd) > 0.0f;
	const bool has_specular = MaxComponent(surf.f0) > 0.0f;

	px.pos[i] = surf.p;
	px.seen[i] = travelled > 0.0f ? cam.origin + eye_dir * (travelled + hit.t) : surf.p;
	px.bent[i] = travelled > 0.0f;
	if (!px.bent[i] && (hit.tri & kDynamic) && !sc.frame->prev.empty())
	{
		const Vec3 *corner = &sc.frame->prev[(size_t)(hit.tri & ~kDynamic) * 3];
		const Vec3 before = corner[0] * (1.0f - hit.u - hit.v) + corner[1] * hit.u + corner[2] * hit.v;
		const Vec3 shift = before - surf.p;
		if (Dot(shift, shift) > 1e-6f)
		{
			// history is looked up where this point was, not where it is
			px.seen[i] = before;
			px.moved[i] = 1;
		}
	}
	px.plane[i] = surf.ng;
	px.normal[i] = surf.n;
	px.depth[i] = travelled + hit.t;
	px.roughness[i] = surf.roughness;
	px.albedo[kDiffuse][i] = kd * tint;
	px.albedo[kSpecular][i] = ks * tint;

	// exact, so it skips the filters; the balls of light come with the map's lights below
	const Lit flash = DirectFrameAll(sc, surf, rng);
	// (the matte part has what the shine leaves here too, by the shine's
	// share on the whole, which has no noise in it)
	const Vec3 flash_light = (surf.kd * (Vec3(1, 1, 1) - surf.SpecularAlbedo()) * flash.diffuse * kInvPi + flash.specular) * tint * through;
	px.add[i] = front_add + flash_light;
	if (mat.emissive && surf.front)
		px.add[i] += Emitted(surf, true) * tint * through;
	px.add[i] *= direct_on;

	// Air that scatters light: some of what the surface sends is lost on the
	// way, and the air itself glows where light falls through it, which is
	// what shows as shafts. A few points along the way are sampled per frame.
	if (sc.fog_density > 0.0f)
	{
		const float reach = have_layer ? first_layer_depth : hit.t;		// the straight part of the view
		const Vec3 glow = FogGlow(sc, cam.origin, eye_dir, reach, rng) * direct_on;
		const float glow_lum = Luminance(glow);
		px.albedo[kFog][i] = Vec3(1, 1, 1);
		px.light[kFog][i] = glow;
		px.m1[kFog][i] = glow_lum;
		px.m2[kFog][i] = glow_lum * glow_lum;

		const float kept = std::exp(-sc.fog_density * px.depth[i]);
		px.albedo[kDiffuse][i] *= kept;
		px.albedo[kSpecular][i] *= kept;
		px.albedo[kOver][i] *= kept;
		px.add[i] *= kept;
	}
	if (shafted)
	{
		// with the air's: it is the same kind of light
		px.albedo[kFog][i] = Vec3(1, 1, 1);
		px.light[kFog][i] += shaft * direct_on;
		px.m1[kFog][i] = Luminance(px.light[kFog][i]);
		px.m2[kFog][i] = px.m1[kFog][i] * px.m1[kFog][i];
	}
	// what the frame's lights put here, to be compared with last frame's
	px.flash[i] = FrameLightLevel(sc, surf,
		flash_light * (direct_on * (sc.fog_density > 0.0f ? std::exp(-sc.fog_density * px.depth[i]) : 1.0f)),
		cam.origin, eye_dir, have_layer ? first_layer_depth : hit.t);

	// a specular path costs as much as a diffuse one; where the lobe reflects
	// little, take it only some of the time
	const float spec_chance = (!has_specular || sc.reflections < 2) ? 0.0f
		: std::min(1.0f, std::max(0.1f, Luminance(surf.SpecularAlbedo()) * 10.0f) * sc.reflection_rate);

	Vec3 sum[2];
	float m1[2] = {}, m2[2] = {};
	float spec_reach = -1.0f;
	for (int k = 0; k < samples; k++)
	{
		Vec3 c[2];

		// The shine takes its share of the light first and the matte part
		// has what is left, so the two together never reflect more than
		// falls on them: see Radiance. The share is that of one way the
		// light could be mirrored, which is the way followed below.
		Vec3 mirrored, shine;
		if (!has_specular || !SampleSpecular(surf, rng, mirrored, shine))
			shine = Vec3();
		const Vec3 left = Vec3(1, 1, 1) - shine;

		const Lit direct = DirectWorld(sc, surf, rng, true), ball = DirectFrameBall(sc, surf, rng);
		c[kDiffuse] = left * (direct.diffuse + ball.diffuse) * (kInvPi * direct_on);
		c[kSpecular] = Demodulate(direct.specular + ball.specular, spec_albedo) * direct_on;
		int followed[2] = {};		// rays the diffuse and the specular path were made of

		if (bounces > 0 && indirect_on > 0.0f)
		{
			Ray bounce;
			bounce.o = surf.p + surf.ng * kRayOffset;
			bounce.tmin = 0.0f;
			bounce.tmax = FLT_MAX;

			// The reflection is followed only some of the time, and counts
			// for more when it is. The matte part gives up as much as the
			// reflection counts for, and nothing when it is not followed, so
			// that the two still add up; the chance is never less than the
			// share, or the matte part would have to give up more than it has.
			const float chance = spec_chance > 0.0f ? std::min(1.0f, std::max(spec_chance, MaxComponent(shine))) : 0.0f;
			const bool follow = chance > 0.0f && rng.Float() < chance && MaxComponent(shine) > 0.0f;
			if (has_diffuse)
			{
				bounce.d = SampleDiffuse(surf, rng);
				const Vec3 kept = follow ? Vec3(1, 1, 1) - shine * (1.0f / chance) : Vec3(1, 1, 1);
				c[kDiffuse] += kept * Radiance(sc, bounce, rng, false, false, 1, bounces, nullptr, &followed[0]);
			}
			if (follow)
			{
				float reached = 0.0f;
				bounce.d = mirrored;
				c[kSpecular] += Demodulate(
					shine * Radiance(sc, bounce, rng, false, !surf.light_sampled_spec, 1, sc.reflection_bounces, &reached,
						&followed[1]),
					spec_albedo) * (1.0f / chance);
				spec_reach = reached;
			}
		}
		else if (furnace)
			c[kDiffuse] += Vec3(PT_FURNACE_LIGHT);		// a path with no bounces to run out of
		if (view_mode == PT_VIEW_BOUNCES)
		{
			// the count in place of the light, to be gathered and filtered as light is
			c[kDiffuse] = Vec3((float)std::max(followed[0], followed[1]));
			c[kSpecular] = Vec3();
		}

		for (int ch = 0; ch < 2; ch++)
		{
			c[ch] = ClampSample(c[ch], sc.max_sample);
			const float lum = Luminance(c[ch]);
			sum[ch] += c[ch];
			m1[ch] += lum;
			m2[ch] += lum * lum;
		}
	}

	// A reflection in something close to a mirror appears to sit behind the
	// surface, as far again as the thing reflected is in front of it. That is
	// where to look for it in last frame's picture.
	if (spec_reach >= 0.0f && surf.roughness < kLightSampledRoughness && !px.moved[i] && !px.bent[i])
	{
		px.spec_pos[i] = cam.origin + eye_dir * (hit.t + std::min(spec_reach, 100000.0f));
		px.spec_ok[i] = 1;
	}

	const float inv = 1.0f / samples;
	for (int ch = 0; ch < 2; ch++)
	{
		px.light[ch][i] = sum[ch] * inv;
		px.m1[ch][i] = m1[ch] * inv;
		px.m2[ch][i] = m2[ch] * inv;
	}
	if (view_mode == PT_VIEW_COST)
	{
		// the rays in place of the light, to be gathered and filtered as light is
		const float rays = (float)rng.rays;
		px.light[kDiffuse][i] = Vec3(rays);
		px.m1[kDiffuse][i] = rays;
		px.m2[kDiffuse][i] = rays * rays;
		px.light[kSpecular][i] = Vec3();
		px.m1[kSpecular][i] = px.m2[kSpecular][i] = 0.0f;
	}
}

// Is what pixel q showed last frame the same surface pixel i shows now?
// Normally: on the same plane, facing the same way. Seen through water the
// view wobbles with the ripples and lands on different spots of a bed that
// need not be flat, so there the test is only that it is about as far away.
bool SameSurface(const Pixels &cur, const Pixels &prev, size_t i, size_t q)
{
	if (cur.bent[i])
		return prev.bent[q] && std::fabs(prev.depth[q] - cur.depth[i]) < 0.1f * cur.depth[i] + 1.0f;
	if (cur.moved[i])
	{
		// cur.seen is where this point was last frame: was that what q showed?
		const Vec3 off = prev.pos[q] - cur.seen[i];
		const float slack = 2.0f + cur.depth[i] * 0.02f;
		return Dot(off, off) <= slack * slack;
	}
	return !prev.bent[q]
		&& std::fabs(Dot(cur.plane[i], prev.pos[q] - cur.pos[i])) <= 1.0f + cur.depth[i] * 0.01f
		&& Dot(cur.plane[i], prev.plane[q]) >= 0.9f;
}

// Blends this frame's samples into what earlier frames saw of the same surface
void Accumulate(CpuBackend *s, const Camera &prev_cam, float max_history, int y)
{
	const int rw = s->rw, rh = s->rh;
	Pixels &cur = s->cur;
	const Pixels &prev = s->prev;

	for (int x = 0; x < rw; x++)
	{
		const size_t i = (size_t)y * rw + x;
		cur.length[i] = 0.0f;
		cur.over_length[i] = 0.0f;
		for (int c = 0; c < kChannels; c++)
			cur.variance[c][i] = 0.0f;
		if (cur.depth[i] < 0.0f)
			continue;

		const Vec3 spec_sample = cur.light[kSpecular][i];
		const float spec_m1 = cur.m1[kSpecular][i], spec_m2 = cur.m2[kSpecular][i];
		const bool has_over = cur.albedo[kOver][i].x > 0.0f;
		const Vec3 over_sample = cur.light[kOver][i];
		const float over_m1 = cur.m1[kOver][i], over_m2 = cur.m2[kOver][i];

		Vec3 hist[kChannels];
		float hm1[kChannels] = {}, hm2[kChannels] = {}, hlen = 0.0f, wsum = 0.0f, hflash = 0.0f;
		bool have_spot = false;
		int spot_x = 0, spot_y = 0;	// the pixel this point was nearest to last frame

		// Where frames are only added up with the eye at rest, what moves
		// starts afresh, so that nothing of where it was shows.
		if (s->have_history && !(s->filtering == 1 && cur.moved[i]))
		{
			// where was this point on screen last frame?
			const Vec3 v = cur.seen[i] - prev_cam.origin;
			const float z = Dot(v, prev_cam.forward);
			if (z > 0.01f)
			{
				// Less this frame's offset within the pixel: history belongs to
				// pixels, and with the eye at rest must come from the very same
				// one, or it would be resampled and crawl about every frame.
				const float fx = (Dot(v, prev_cam.right) / (z * prev_cam.tx) * 0.5f + 0.5f) * rw - 0.5f - s->jitter_x;
				const float fy = (0.5f - Dot(v, prev_cam.up) / (z * prev_cam.ty) * 0.5f) * rh - 0.5f - s->jitter_y;
				const int ix = (int)std::floor(fx + 0.001f), iy = (int)std::floor(fy + 0.001f);
				have_spot = true;
				spot_x = (int)std::floor(fx + 0.5f);
				spot_y = (int)std::floor(fy + 0.5f);
				const float ax = fx - ix, ay = fy - iy;

				for (int t = 0; t < 4; t++)
				{
					const int qx = ix + (t & 1), qy = iy + (t >> 1);
					if (qx < 0 || qy < 0 || qx >= rw || qy >= rh)
						continue;
					const size_t q = (size_t)qy * rw + qx;
					if (prev.depth[q] < 0.0f || prev.length[q] <= 0.0f)
						continue;
					if (!SameSurface(cur, prev, i, q))
						continue;
					const float w = ((t & 1) ? ax : 1.0f - ax) * ((t >> 1) ? ay : 1.0f - ay);
					if (w <= 0.0f)
						continue;
					for (int c = 0; c < kChannels; c++)
					{
						hist[c] += prev.light[c][q] * w;
						hm1[c] += prev.m1[c][q] * w;
						hm2[c] += prev.m2[c][q] * w;
					}
					hflash += prev.flash[q] * w;
					hlen += prev.length[q] * w;
					wsum += w;
				}
			}
		}

		if (wsum <= 0.01f && s->have_history && have_spot)
		{
			// Nothing usable right where this point was. Along an edge a
			// pixel shows one surface on some frames and its neighbour on
			// others, so the same surface is usually one pixel away.
			for (int dy = -1; dy <= 1; dy++)
			{
				for (int dx = -1; dx <= 1; dx++)
				{
					const int qx = spot_x + dx, qy = spot_y + dy;
					if (qx < 0 || qy < 0 || qx >= rw || qy >= rh)
						continue;
					const size_t q = (size_t)qy * rw + qx;
					if (prev.depth[q] < 0.0f || prev.length[q] <= 0.0f)
						continue;
					if (!SameSurface(cur, prev, i, q))
						continue;
					for (int c = 0; c < kChannels; c++)
					{
						hist[c] += prev.light[c][q];
						hm1[c] += prev.m1[c][q];
						hm2[c] += prev.m2[c][q];
					}
					hflash += prev.flash[q];
					hlen += prev.length[q];
					wsum += 1.0f;
				}
			}
		}

		float len = 1.0f;
		if (wsum <= 0.01f && s->have_history)
		{
			// Nothing where this point used to be: it may belong to something
			// that moves with the eye, like the weapon in hand. If the same
			// pixel showed much the same surface last frame, carry on from
			// that, but not for long, since it is a guess.
			if (prev.depth[i] > 0.0f && prev.length[i] > 0.0f &&
				std::fabs(prev.depth[i] - cur.depth[i]) < 0.1f * cur.depth[i] &&
				Dot(cur.plane[i], prev.plane[i]) > 0.8f)
			{
				for (int c = 0; c < kChannels; c++)
				{
					hist[c] = prev.light[c][i];
					hm1[c] = prev.m1[c][i];
					hm2[c] = prev.m2[c][i];
				}
				hflash = prev.flash[i];
				hlen = std::min(prev.length[i], 8.0f);
				wsum = 1.0f;
			}
		}
		float changed = 0.0f;
		if (wsum > 0.01f)
		{
			const float inv = 1.0f / wsum;
			len = std::min(hlen * inv + 1.0f, max_history);
			// By how much of the pixel's whole light what the frame's lights
			// put on it has changed: the rest of the light is what was
			// gathered, as it will be seen. A change below a fortieth is let
			// be: a light moving far off, or in another room, shifts the air's
			// glow a little everywhere, as it is reckoned with nothing in the
			// way. Above that, history is kept for the inverse of the change
			// at most, taken four times over: a change of a quarter or more
			// starts afresh, one of a tenth keeps three frames or so, since
			// what the rest of the light did was most likely the same, and
			// what is kept of its old value is an error of that size.
			float rest = 0.0f;
			for (int c = 0; c < kChannels; c++)
				rest += Luminance(cur.albedo[c][i] * hist[c]) * inv;
			const float flash_was = hflash * inv;
			changed = std::min(4.0f * std::max(std::fabs(cur.flash[i] - flash_was) / (std::max(cur.flash[i], flash_was) + rest + 1e-4f) - 0.025f, 0.0f), 1.0f);
			if (changed > 0.0f)
				len = std::min(len, 1.0f / changed);
			for (int c = 0; c < kChannels; c++)
			{
				// a mirror shows something else as soon as the eye moves, so
				// smooth reflections keep less of the past while things change
				float keep = len;
				if (c == kSpecular && max_history <= s->moving_history)
					keep = std::min(len, std::max(2.0f, s->moving_history * cur.roughness[i] * 2.0f));
				// and the air's light is followed by the surface behind it,
				// which is not where it is: it keeps less too
				if (c == kFog && max_history <= s->moving_history)
					keep = std::min(len, s->fog_history);
				const float a = 1.0f / keep;
				const Vec3 h = hist[c] * inv;
				const float h1 = hm1[c] * inv, h2 = hm2[c] * inv;
				cur.light[c][i] = h + (cur.light[c][i] - h) * a;
				cur.m1[c][i] = h1 + (cur.m1[c][i] - h1) * a;
				cur.m2[c][i] = h2 + (cur.m2[c][i] - h2) * a;
			}
		}
		cur.length[i] = len;

		// how unsure the average still is; with little history, assume very
		for (int c = 0; c < kChannels; c++)
		{
			const float n = (c == kFog && max_history <= s->moving_history) ? std::min(len, s->fog_history) : len;
			float var = std::max(0.0f, cur.m2[c][i] - cur.m1[c][i] * cur.m1[c][i]) / n;
			if (len < 4.0f)
				var = std::max(var, cur.m1[c][i] * cur.m1[c][i] * 0.25f + 0.01f);
			cur.variance[c][i] = var;
		}

		// The same goes for what a mirror-like solid surface reflects.
		if (cur.spec_ok[i] && s->have_history && s->reflection_history)
		{
			const Vec3 v = cur.spec_pos[i] - prev_cam.origin;
			const float z = Dot(v, prev_cam.forward);
			Vec3 sh;
			float sh1 = 0.0f, sh2 = 0.0f, sw = 0.0f;
			if (z > 0.01f)
			{
				const float fx = (Dot(v, prev_cam.right) / (z * prev_cam.tx) * 0.5f + 0.5f) * rw - 0.5f - s->jitter_x;
				const float fy = (0.5f - Dot(v, prev_cam.up) / (z * prev_cam.ty) * 0.5f) * rh - 0.5f - s->jitter_y;
				const int ix = (int)std::floor(fx + 0.001f), iy = (int)std::floor(fy + 0.001f);
				const float ax = fx - ix, ay = fy - iy;

				for (int t = 0; t < 4; t++)
				{
					const int qx = ix + (t & 1), qy = iy + (t >> 1);
					if (qx < 0 || qy < 0 || qx >= rw || qy >= rh)
						continue;
					const size_t q = (size_t)qy * rw + qx;
					// any surface about as smooth, facing much the same way
					if (prev.depth[q] < 0.0f || prev.length[q] <= 0.0f
						|| std::fabs(prev.roughness[q] - cur.roughness[i]) > 0.1f
						|| Dot(cur.plane[i], prev.plane[q]) < 0.8f)
						continue;
					const float w = ((t & 1) ? ax : 1.0f - ax) * ((t >> 1) ? ay : 1.0f - ay);
					if (w <= 0.0f)
						continue;
					sh += prev.light[kSpecular][q] * w;
					sh1 += prev.m1[kSpecular][q] * w;
					sh2 += prev.m2[kSpecular][q] * w;
					sw += w;
				}
			}
			if (sw > 0.01f)
			{
				const float inv = 1.0f / sw;
				float n = std::min(std::max(cur.length[i], 2.0f), max_history);
				if (changed > 0.0f)
					n = std::min(n, std::max(1.0f / changed, 1.0f));
				const float a = 1.0f / n;
				cur.light[kSpecular][i] = sh * inv + (spec_sample - sh * inv) * a;
				cur.m1[kSpecular][i] = sh1 * inv + (spec_m1 - sh1 * inv) * a;
				cur.m2[kSpecular][i] = sh2 * inv + (spec_m2 - sh2 * inv) * a;
				cur.variance[kSpecular][i] = std::max(0.0f,
					cur.m2[kSpecular][i] - cur.m1[kSpecular][i] * cur.m1[kSpecular][i]) / n;
			}
		}

		// Reflections in glass and water do not move across the screen the
		// way the surface behind them does, so their history is looked up
		// where they appear to be instead.
		if (has_over && s->reflection_history)
		{
			Vec3 oh;
			float oh1 = 0.0f, oh2 = 0.0f, olen = 0.0f, ow = 0.0f;
			if (s->have_history)
			{
				const Vec3 v = cur.over_pos[i] - prev_cam.origin;
				const float z = Dot(v, prev_cam.forward);
				if (z > 0.01f)
				{
					const float fx = (Dot(v, prev_cam.right) / (z * prev_cam.tx) * 0.5f + 0.5f) * rw - 0.5f - s->jitter_x;
					const float fy = (0.5f - Dot(v, prev_cam.up) / (z * prev_cam.ty) * 0.5f) * rh - 0.5f - s->jitter_y;
					const int ix = (int)std::floor(fx + 0.001f), iy = (int)std::floor(fy + 0.001f);
					const float ax = fx - ix, ay = fy - iy;

					for (int t = 0; t < 4; t++)
					{
						const int qx = ix + (t & 1), qy = iy + (t >> 1);
						if (qx < 0 || qy < 0 || qx >= rw || qy >= rh)
							continue;
						const size_t q = (size_t)qy * rw + qx;
						// Any layer light found there will do. How far away a
						// reflection appears to be changes from frame to frame on
						// rippling water, so it cannot be used to tell reflections apart.
						if (prev.over_length[q] <= 0.0f)
							continue;
						const float w = ((t & 1) ? ax : 1.0f - ax) * ((t >> 1) ? ay : 1.0f - ay);
						if (w <= 0.0f)
							continue;
						oh += prev.light[kOver][q] * w;
						oh1 += prev.m1[kOver][q] * w;
						oh2 += prev.m2[kOver][q] * w;
						olen += prev.over_length[q] * w;
						ow += w;
					}
				}
			}

			float n = 1.0f;
			cur.light[kOver][i] = over_sample;
			cur.m1[kOver][i] = over_m1;
			cur.m2[kOver][i] = over_m2;
			if (ow > 0.01f)
			{
				const float inv = 1.0f / ow;
				n = std::min(olen * inv + 1.0f, max_history);
				if (changed > 0.0f)
					n = std::min(n, std::max(1.0f / changed, 1.0f));
				const float a = 1.0f / n;
				cur.light[kOver][i] = oh * inv + (over_sample - oh * inv) * a;
				cur.m1[kOver][i] = oh1 * inv + (over_m1 - oh1 * inv) * a;
				cur.m2[kOver][i] = oh2 * inv + (over_m2 - oh2 * inv) * a;
			}
			cur.over_length[i] = n;

			float var = std::max(0.0f, cur.m2[kOver][i] - cur.m1[kOver][i] * cur.m1[kOver][i]) / n;
			if (n < 4.0f)
				var = std::max(var, cur.m1[kOver][i] * cur.m1[kOver][i] * 0.25f + 0.01f);
			cur.variance[kOver][i] = var;
		}
	}
}

// One pass of an a-trous wavelet filter. Neighbours count for less the more
// they differ in plane, facing or brightness, and brightness matters less
// where the estimate is still noisy. Sharp reflections are left sharper.
#ifdef PT_AVX2_KERNELS
// Eight pixels of the row ride in the lanes of one AVX register. The sums
// are those the other build makes, pixel by pixel and in its order, with
// the arithmetic the compiler makes of that code for AVX2, fused
// multiply-adds where it fuses them: the picture is the same to the last
// bit. Where that code passes a neighbour over, this one works it out for
// all eight and leaves the sums of the pixels it does not count for alone.
void FilterRow(int rw, int rh, const FilterRows &geo, const FilterRows &in, FilterRows &out, int step, int y)
{
	static const float kernel[5] = {1.0f / 16, 1.0f / 4, 3.0f / 8, 1.0f / 4, 1.0f / 16};
	// how big a brightness difference is still taken for noise, per channel;
	// the specular one goes by roughness
	static const float tolerances[kChannels] = {4.0f, 0.0f, 0.75f, 6.0f};
	const __m256 lum_r = _mm256_set1_ps(0.2126f), lum_g = _mm256_set1_ps(0.7152f), lum_b = _mm256_set1_ps(0.0722f);
	const __m256 sign = _mm256_set1_ps(-0.0f), zero = _mm256_setzero_ps(), one = _mm256_set1_ps(1.0f);
	const __m256i lanes = _mm256_setr_epi32(0, 1, 2, 3, 4, 5, 6, 7);
	const size_t gs = geo.stride, ls = in.stride;
	const float w0 = kernel[2] * kernel[2];

	for (int x = 0; x < rw; x += 8)
	{
		const float *g = geo.Row(y) + x;
		const float *centre = in.Row(y) + x;
		float *to = out.Row(y) + x;

		// Pixels to be left as they are: those past the end of the row, those
		// with no surface, and those settled already, with nothing to gain
		// from blurring them
		const __m256 depth = _mm256_loadu_ps(g + kGeoDepth * gs);
		__m256 lum[kChannels], sigma[kChannels];
		__m256 leave = _mm256_cmp_ps(zero, zero, _CMP_EQ_OQ);
		for (int c = 0; c < kChannels; c++)
		{
			const float *l = centre + 4 * c * ls;
			const __m256 cr = _mm256_loadu_ps(l), cg = _mm256_loadu_ps(l + ls), cb = _mm256_loadu_ps(l + 2 * ls);
			lum[c] = _mm256_fmadd_ps(cg, lum_g, _mm256_fmadd_ps(cr, lum_r, _mm256_mul_ps(cb, lum_b)));
			sigma[c] = _mm256_sqrt_ps(_mm256_loadu_ps(l + 3 * ls));
			leave = _mm256_and_ps(leave, _mm256_cmp_ps(sigma[c],
				_mm256_fmadd_ps(lum[c], _mm256_set1_ps(0.01f), _mm256_set1_ps(1e-4f)), _CMP_LE_OS));
		}
		leave = _mm256_or_ps(leave, _mm256_cmp_ps(depth, zero, _CMP_NGE_UQ));
		leave = _mm256_or_ps(leave, _mm256_castsi256_ps(_mm256_cmpgt_epi32(lanes, _mm256_set1_epi32(rw - 1 - x))));
		if (_mm256_movemask_ps(leave) == 255)
		{
			for (int q = 0; q < 4 * kChannels; q++)
				_mm256_storeu_ps(to + q * ls, _mm256_loadu_ps(centre + q * ls));
			continue;
		}

		// What a neighbour counts for by where it is and how it faces is the
		// same for every channel: that first, for the neighbours that count
		// for any of the eight
		const __m256 gpx = _mm256_loadu_ps(g + kGeoPosX * gs), gpy = _mm256_loadu_ps(g + kGeoPosY * gs);
		const __m256 gpz = _mm256_loadu_ps(g + kGeoPosZ * gs);
		const __m256 gnx = _mm256_loadu_ps(g + kGeoNormalX * gs), gny = _mm256_loadu_ps(g + kGeoNormalY * gs);
		const __m256 gnz = _mm256_loadu_ps(g + kGeoNormalZ * gs);
		const __m256 glx = _mm256_loadu_ps(g + kGeoPlaneX * gs), gly = _mm256_loadu_ps(g + kGeoPlaneY * gs);
		const __m256 glz = _mm256_loadu_ps(g + kGeoPlaneZ * gs);
		const __m256 roughness = _mm256_loadu_ps(g + kGeoRoughness * gs);
		const __m256 inv_plane = _mm256_div_ps(one, _mm256_fmadd_ps(_mm256_set1_ps(0.004f), depth, one));

		__m256 counts[24], weight[24], wz[24], wr[24];
		const float *from[24];
		int taps = 0;
		for (int dy = -2; dy <= 2; dy++)
		{
			const int qy = y + dy * step;
			if (qy < 0 || qy >= rh)
				continue;
			for (int dx = -2; dx <= 2; dx++)
			{
				const int qx = x + dx * step;
				if (qx <= -8 || qx >= rw || (!dx && !dy))
					continue;
				const float *h = geo.Row(qy) + qx;

				const __m256i at = _mm256_add_epi32(lanes, _mm256_set1_epi32(qx));
				__m256 ok = _mm256_castsi256_ps(_mm256_and_si256(
					_mm256_cmpgt_epi32(at, _mm256_set1_epi32(-1)), _mm256_cmpgt_epi32(_mm256_set1_epi32(rw), at)));
				ok = _mm256_andnot_ps(leave, ok);
				ok = _mm256_and_ps(ok, _mm256_cmp_ps(_mm256_loadu_ps(h + kGeoDepth * gs), zero, _CMP_NLT_UQ));

				const __m256 wn = _mm256_fmadd_ps(_mm256_loadu_ps(h + kGeoNormalZ * gs), gnz,
					_mm256_fmadd_ps(_mm256_loadu_ps(h + kGeoNormalX * gs), gnx,
						_mm256_mul_ps(gny, _mm256_loadu_ps(h + kGeoNormalY * gs))));
				ok = _mm256_and_ps(ok, _mm256_cmp_ps(wn, zero, _CMP_GT_OQ));

				const __m256 px = _mm256_sub_ps(_mm256_loadu_ps(h + kGeoPosX * gs), gpx);
				const __m256 py = _mm256_sub_ps(_mm256_loadu_ps(h + kGeoPosY * gs), gpy);
				const __m256 pz = _mm256_sub_ps(_mm256_loadu_ps(h + kGeoPosZ * gs), gpz);
				const __m256 off = _mm256_fmadd_ps(pz, glz, _mm256_fmadd_ps(glx, px, _mm256_mul_ps(py, gly)));
				const __m256 z = _mm256_mul_ps(_mm256_andnot_ps(sign, off), inv_plane);
				ok = _mm256_and_ps(ok, _mm256_cmp_ps(z, _mm256_set1_ps(4.0f), _CMP_NGE_UQ));
				if (!_mm256_movemask_ps(ok))
					continue;

				const __m256 wn2 = _mm256_mul_ps(wn, wn), wn4 = _mm256_mul_ps(wn2, wn2);		// ^8 below
				counts[taps] = ok;
				weight[taps] = _mm256_mul_ps(_mm256_set1_ps(kernel[dx + 2] * kernel[dy + 2]), _mm256_mul_ps(wn4, wn4));
				wz[taps] = z;
				wr[taps] = _mm256_mul_ps(_mm256_andnot_ps(sign,
					_mm256_sub_ps(_mm256_loadu_ps(h + kGeoRoughness * gs), roughness)), _mm256_set1_ps(8.0f));	// specular only
				from[taps] = in.Row(qy) + qx;
				taps++;
			}
		}

		// then the sums, a channel at a time
		for (int c = 0; c < kChannels; c++)
		{
			const size_t channel = 4 * c * ls;
			const __m256 cr = _mm256_loadu_ps(centre + channel), cg = _mm256_loadu_ps(centre + channel + ls);
			const __m256 cb = _mm256_loadu_ps(centre + channel + 2 * ls), cvar = _mm256_loadu_ps(centre + channel + 3 * ls);
			const __m256 tolerance = c == kSpecular ?
				_mm256_fmadd_ps(_mm256_set1_ps(3.0f), roughness, one) : _mm256_set1_ps(tolerances[c]);
			const __m256 inv_lum = _mm256_div_ps(one, _mm256_fmadd_ps(tolerance, sigma[c], _mm256_set1_ps(1e-3f)));
			const __m256 clum = lum[c];

			__m256 wsum = _mm256_set1_ps(w0);
			__m256 sr = _mm256_mul_ps(cr, wsum), sg = _mm256_mul_ps(cg, wsum), sb = _mm256_mul_ps(cb, wsum);
			__m256 vsum = _mm256_mul_ps(cvar, _mm256_set1_ps(w0 * w0));

			for (int t = 0; t < taps; t++)
			{
				const float *q = from[t] + channel;
				const __m256 qr = _mm256_loadu_ps(q), qg = _mm256_loadu_ps(q + ls), qb = _mm256_loadu_ps(q + 2 * ls);
				const __m256 qlum = _mm256_fmadd_ps(qg, lum_g, _mm256_fmadd_ps(qr, lum_r, _mm256_mul_ps(qb, lum_b)));

				// weight * exp(-(wz + wl + wr)), with the exponential as (1 - x/4)^4
				__m256 e = _mm256_fmadd_ps(_mm256_andnot_ps(sign, _mm256_sub_ps(qlum, clum)), inv_lum, wz[t]);
				if (c == kSpecular)
					e = _mm256_add_ps(e, wr[t]);
				e = _mm256_max_ps(zero, _mm256_fnmadd_ps(e, _mm256_set1_ps(0.25f), one));
				e = _mm256_mul_ps(e, e);
				e = _mm256_mul_ps(e, e);
				const __m256 w = _mm256_mul_ps(weight[t], e);

				const __m256 ok = counts[t];
				sr = _mm256_blendv_ps(sr, _mm256_fmadd_ps(w, qr, sr), ok);
				sg = _mm256_blendv_ps(sg, _mm256_fmadd_ps(w, qg, sg), ok);
				sb = _mm256_blendv_ps(sb, _mm256_fmadd_ps(w, qb, sb), ok);
				vsum = _mm256_blendv_ps(vsum, _mm256_fmadd_ps(_mm256_mul_ps(w, w), _mm256_loadu_ps(q + 3 * ls), vsum), ok);
				wsum = _mm256_blendv_ps(wsum, _mm256_add_ps(w, wsum), ok);
			}

			const __m256 inv = _mm256_div_ps(one, wsum);
			_mm256_storeu_ps(to + channel, _mm256_blendv_ps(_mm256_mul_ps(inv, sr), cr, leave));
			_mm256_storeu_ps(to + channel + ls, _mm256_blendv_ps(_mm256_mul_ps(inv, sg), cg, leave));
			_mm256_storeu_ps(to + channel + 2 * ls, _mm256_blendv_ps(_mm256_mul_ps(inv, sb), cb, leave));
			_mm256_storeu_ps(to + channel + 3 * ls,
				_mm256_blendv_ps(_mm256_mul_ps(_mm256_mul_ps(inv, inv), vsum), cvar, leave));
		}
	}
}
#else
// The three channels ride in the lanes of one SSE register.
void FilterRow(int rw, int rh, const FilterGeo *geo, const FilterLight *in, FilterLight *out, int step, int y)
{
	static const float kernel[5] = {1.0f / 16, 1.0f / 4, 3.0f / 8, 1.0f / 4, 1.0f / 16};
	const __m128 lum_r = _mm_set1_ps(0.2126f), lum_g = _mm_set1_ps(0.7152f), lum_b = _mm_set1_ps(0.0722f);
	const __m128 sign = _mm_set1_ps(-0.0f), zero = _mm_setzero_ps(), one = _mm_set1_ps(1.0f);

	for (int x = 0; x < rw; x++)
	{
		const size_t i = (size_t)y * rw + x;
		const FilterGeo &g = geo[i];
		const FilterLight &centre = in[i];
		if (g.depth < 0.0f)
		{
			out[i] = centre;
			continue;
		}

		const __m128 cr = _mm_loadu_ps(centre.r), cg = _mm_loadu_ps(centre.g), cb = _mm_loadu_ps(centre.b);
		const __m128 cvar = _mm_loadu_ps(centre.var);
		const __m128 lum = _mm_add_ps(_mm_add_ps(_mm_mul_ps(cr, lum_r), _mm_mul_ps(cg, lum_g)), _mm_mul_ps(cb, lum_b));
		const __m128 sigma = _mm_sqrt_ps(cvar);

		// settled already: nothing to gain from blurring it
		if (_mm_movemask_ps(_mm_cmple_ps(sigma, _mm_add_ps(_mm_mul_ps(lum, _mm_set1_ps(0.01f)), _mm_set1_ps(1e-4f)))) == 15)
		{
			out[i] = centre;
			continue;
		}

		// how big a brightness difference is still taken for noise, per channel
		const __m128 tolerance = _mm_setr_ps(4.0f, 1.0f + 3.0f * g.roughness, 0.75f, 6.0f);
		const __m128 inv_lum = _mm_div_ps(one, _mm_add_ps(_mm_mul_ps(tolerance, sigma), _mm_set1_ps(1e-3f)));
		const float inv_plane = 1.0f / (1.0f + g.depth * 0.004f);

		const float w0 = kernel[2] * kernel[2];
		__m128 wsum = _mm_set1_ps(w0);
		__m128 sr = _mm_mul_ps(cr, wsum), sg = _mm_mul_ps(cg, wsum), sb = _mm_mul_ps(cb, wsum);
		__m128 vsum = _mm_mul_ps(cvar, _mm_set1_ps(w0 * w0));

		for (int dy = -2; dy <= 2; dy++)
		{
			const int qy = y + dy * step;
			if (qy < 0 || qy >= rh)
				continue;
			for (int dx = -2; dx <= 2; dx++)
			{
				const int qx = x + dx * step;
				if (qx < 0 || qx >= rw || (!dx && !dy))
					continue;
				const size_t q = (size_t)qy * rw + qx;
				const FilterGeo &h = geo[q];
				if (h.depth < 0.0f)
					continue;

				float wn = Dot(g.normal, h.normal);
				if (wn <= 0.0f)
					continue;
				wn *= wn; wn *= wn; wn *= wn;				// ^8
				const float wz = std::fabs(Dot(g.plane, h.pos - g.pos)) * inv_plane;
				if (wz >= 4.0f)
					continue;
				const float wr = std::fabs(h.roughness - g.roughness) * 8.0f;	// specular only

				const FilterLight &l = in[q];
				const __m128 qr = _mm_loadu_ps(l.r), qg = _mm_loadu_ps(l.g), qb = _mm_loadu_ps(l.b);
				const __m128 qlum = _mm_add_ps(_mm_add_ps(_mm_mul_ps(qr, lum_r), _mm_mul_ps(qg, lum_g)), _mm_mul_ps(qb, lum_b));
				const __m128 wl = _mm_mul_ps(_mm_andnot_ps(sign, _mm_sub_ps(qlum, lum)), inv_lum);

				// weight * exp(-(wz + wl + wr)), with the exponential as (1 - x/4)^4
				__m128 e = _mm_add_ps(_mm_add_ps(wl, _mm_set1_ps(wz)), _mm_setr_ps(0.0f, wr, 0.0f, 0.0f));
				e = _mm_max_ps(zero, _mm_sub_ps(one, _mm_mul_ps(e, _mm_set1_ps(0.25f))));
				e = _mm_mul_ps(e, e);
				e = _mm_mul_ps(e, e);
				const __m128 w = _mm_mul_ps(e, _mm_set1_ps(kernel[dx + 2] * kernel[dy + 2] * wn));

				sr = _mm_add_ps(sr, _mm_mul_ps(qr, w));
				sg = _mm_add_ps(sg, _mm_mul_ps(qg, w));
				sb = _mm_add_ps(sb, _mm_mul_ps(qb, w));
				vsum = _mm_add_ps(vsum, _mm_mul_ps(_mm_loadu_ps(l.var), _mm_mul_ps(w, w)));
				wsum = _mm_add_ps(wsum, w);
			}
		}

		const __m128 inv = _mm_div_ps(one, wsum);
		_mm_storeu_ps(out[i].r, _mm_mul_ps(sr, inv));
		_mm_storeu_ps(out[i].g, _mm_mul_ps(sg, inv));
		_mm_storeu_ps(out[i].b, _mm_mul_ps(sb, inv));
		_mm_storeu_ps(out[i].var, _mm_mul_ps(vsum, _mm_mul_ps(inv, inv)));
	}
}
#endif

// low discrepancy sequence: successive values fill [0, 1) evenly
float Halton(uint32_t index, uint32_t base)
{
	float f = 1.0f, r = 0.0f;
	while (index)
	{
		f /= base;
		r += f * (index % base);
		index /= base;
	}
	return r;
}

// The typical brightness of the picture: the geometric mean of its luminance,
// which a few very bright pixels do not drag about the way an average would
float TypicalLuminance(const CpuBackend *s)
{
	double sum = 0.0;
	int count = 0;
	for (int y = 1; y < s->rh; y += 3)
	{
		for (int x = 1; x < s->rw; x += 3)
		{
			sum += std::log(Luminance(s->hdr[(size_t)y * s->rw + x]) + 1e-4f);
			count++;
		}
	}
	return count ? (float)std::exp(sum / count) : 0.0f;
}

// How far over white a pixel counts as being for the glow, x being how far
// it is: all of it up to half of most, and from there less and less of what
// is left, never reaching most. No limit if most is not above 0.
float BloomExcess(float x, float most)
{
	const float knee = most * 0.5f;
	if (most <= 0.0f || x <= knee)
		return x;
	return knee + (most - knee) * (1.0f - std::exp(-(x - knee) / (most - knee)));
}

// Glow around what is brighter than the screen can show. Works on a half
// size copy: the bright part is taken out, blurred widely, and added back.
void Bloom(CpuBackend *s, float strength, float most)
{
	const int rw = s->rw, rh = s->rh;
	const int bw = std::max(1, rw / 2), bh = std::max(1, rh / 2);
	const size_t count = (size_t)bw * bh;
	if (s->bloom_a.size() != count)
	{
		s->bloom_a.assign(count, Vec3());
		s->bloom_b.assign(count, Vec3());
	}
	std::vector<Vec3> &a = s->bloom_a, &b = s->bloom_b;

	s->pool.Run(bh, [&](int y)
	{
		for (int x = 0; x < bw; x++)
		{
			Vec3 sum;
			for (int k = 0; k < 4; k++)
			{
				const int qx = std::min(rw - 1, x * 2 + (k & 1)), qy = std::min(rh - 1, y * 2 + (k >> 1));
				const Vec3 c = s->hdr[(size_t)qy * rw + qx];
				const float lum = Luminance(c);
				if (lum > 1.0f)
					sum += c * (BloomExcess(lum - 1.0f, most) / lum);
			}
			a[(size_t)y * bw + x] = sum * 0.25f;
		}
	});

	// three box blurs come close to a gaussian
	const int radius = std::max(1, bh / 48);
	const float norm = 1.0f / (2 * radius + 1);
	for (int pass = 0; pass < 3; pass++)
	{
		s->pool.Run(bh, [&](int y)		// across, a into b
		{
			const Vec3 *row = &a[(size_t)y * bw];
			Vec3 run;
			for (int x = -radius; x <= radius; x++)
				run += row[std::min(bw - 1, std::max(0, x))];
			for (int x = 0; x < bw; x++)
			{
				b[(size_t)y * bw + x] = run * norm;
				run += row[std::min(bw - 1, x + radius + 1)] - row[std::max(0, x - radius)];
			}
		});
		s->pool.Run(bw, [&](int x)		// down, b into a
		{
			Vec3 run;
			for (int y = -radius; y <= radius; y++)
				run += b[(size_t)std::min(bh - 1, std::max(0, y)) * bw + x];
			for (int y = 0; y < bh; y++)
			{
				a[(size_t)y * bw + x] = run * norm;
				run += b[(size_t)std::min(bh - 1, y + radius + 1) * bw + x] - b[(size_t)std::max(0, y - radius) * bw + x];
			}
		});
	}

	s->pool.Run(rh, [&](int y)
	{
		const float fy = (y + 0.5f) * 0.5f - 0.5f;
		const int y0 = std::min(bh - 1, std::max(0, (int)std::floor(fy))), y1 = std::min(bh - 1, y0 + 1);
		const float ay = std::min(1.0f, std::max(0.0f, fy - y0));
		for (int x = 0; x < rw; x++)
		{
			const float fx = (x + 0.5f) * 0.5f - 0.5f;
			const int x0 = std::min(bw - 1, std::max(0, (int)std::floor(fx))), x1 = std::min(bw - 1, x0 + 1);
			const float ax = std::min(1.0f, std::max(0.0f, fx - x0));
			const Vec3 glow = (a[(size_t)y0 * bw + x0] * (1.0f - ax) + a[(size_t)y0 * bw + x1] * ax) * (1.0f - ay)
				+ (a[(size_t)y1 * bw + x0] * (1.0f - ax) + a[(size_t)y1 * bw + x1] * ax) * ay;
			s->hdr[(size_t)y * rw + x] += glow * strength;
		}
	});
}

// what the resolve needs to know about the view it is filling
struct Target
{
	int		x, y, width, height;	// the view, in window pixels
	int		x0, y0, x1, y1;			// the part of it inside the window
};

// Without anti-aliasing: stretch the picture to the view, bilinearly
void UpscaleRow(CpuBackend *s, const Target &t, const Grade &grade, int oy)
{
	const int rw = s->rw, rh = s->rh;
	const int wy = t.y + oy;
	if (wy < t.y0 || wy >= t.y1)
		return;

	const float fy = (oy + 0.5f) * rh / t.height - 0.5f;
	const int sy0 = std::min(rh - 1, std::max(0, (int)std::floor(fy))), sy1 = std::min(rh - 1, sy0 + 1);
	const float ay = std::min(1.0f, std::max(0.0f, fy - sy0));
	uint32_t *out = &s->scene[(size_t)wy * s->width];

	for (int ox = 0; ox < t.width; ox++)
	{
		const int wx = t.x + ox;
		if (wx < t.x0 || wx >= t.x1)
			continue;
		const float fx = (ox + 0.5f) * rw / t.width - 0.5f;
		const int sx0 = std::min(rw - 1, std::max(0, (int)std::floor(fx))), sx1 = std::min(rw - 1, sx0 + 1);
		const float ax = std::min(1.0f, std::max(0.0f, fx - sx0));
		const Vec3 c = (s->hdr[(size_t)sy0 * rw + sx0] * (1.0f - ax) + s->hdr[(size_t)sy0 * rw + sx1] * ax) * (1.0f - ay)
			+ (s->hdr[(size_t)sy1 * rw + sx0] * (1.0f - ax) + s->hdr[(size_t)sy1 * rw + sx1] * ax) * ay;
		out[wx] = ToneMap(c, grade);
	}
}

// Temporal anti-aliasing and upscaling in one.
//
// The picture is traced at a lower resolution than the view, and each frame
// through a different point of every traced pixel. A view pixel takes the
// traced sample nearest to it and blends it into a running average kept at
// the view's own resolution, giving it more say the nearer it landed. Over
// a few frames every view pixel has had samples close to it, and the
// average holds detail the traced resolution alone does not. No single
// frame counts for much, so the offsets do not show as shaking.
//
// The average is found by following the point back to where it was on
// screen. It is kept across different surfaces, because along an edge that
// blend is exactly the smooth edge wanted; what keeps stale history out is
// that it may not stray outside what the traced pixels around it show now.
void ResolveRow(CpuBackend *s, const Camera &cam, const Camera &prev_cam, bool have_history, float max_count,
	const Target &t, const Grade &grade, int oy)
{
	const int rw = s->rw, rh = s->rh, vw = t.width, vh = t.height;
	const float to_low_x = (float)rw / vw, to_low_y = (float)rh / vh;
	const float to_view_x = (float)vw / rw, to_view_y = (float)vh / rh;
	const Vec3 *prev = s->steady_prev.data();
	const float *prev_count = s->steady_count_prev.data();
	const Pixels &was = s->prev;
	const int wy = t.y + oy;
	uint32_t *out_row = (wy >= t.y0 && wy < t.y1) ? &s->scene[(size_t)wy * s->width] : nullptr;

	const float ly = (oy + 0.5f) * to_low_y - 0.5f;
	const int sy = std::min(rh - 1, std::max(0, (int)std::floor(ly - s->jitter_y + 0.5f)));
	const float dy = ly - (sy + s->jitter_y);

	for (int ox = 0; ox < vw; ox++)
	{
		// the traced sample nearest this view pixel, and how near
		const float lx = (ox + 0.5f) * to_low_x - 0.5f;
		const int sx = std::min(rw - 1, std::max(0, (int)std::floor(lx - s->jitter_x + 0.5f)));
		const float dx = lx - (sx + s->jitter_x);
		const float nearness = std::max(0.02f, std::exp(-(dx * dx + dy * dy) * 10.0f));

		const size_t i = (size_t)sy * rw + sx;
		const size_t o = (size_t)oy * vw + ox;
		const Vec3 c = s->hdr[i];
		const bool sky = s->cur.depth[i] < 0.0f;
		Vec3 out = c;
		float count = nearness;

		if (have_history)
		{
			// the sky has no position: use a point far along its ray
			Vec3 p = s->cur.seen[i];
			if (sky)
				p = cam.origin + Normalize(cam.forward
					+ cam.right * ((2.0f * (sx + 0.5f) / rw - 1.0f) * cam.tx)
					+ cam.up * ((1.0f - 2.0f * (sy + 0.5f) / rh) * cam.ty)) * 100000.0f;

			// where the traced sample was last frame, less this frame's
			// offset within the pixel (which the sky point was not seen through)
			const Vec3 v = p - prev_cam.origin;
			const float z = Dot(v, prev_cam.forward);
			bool found = false;
			Vec3 h;
			float hcount = 0.0f;

			if (z > 0.01f)
			{
				const float jx = sky ? 0.0f : s->jitter_x, jy = sky ? 0.0f : s->jitter_y;
				const float fx = (Dot(v, prev_cam.right) / (z * prev_cam.tx) * 0.5f + 0.5f) * rw - 0.5f - jx;
				const float fy = (0.5f - Dot(v, prev_cam.up) / (z * prev_cam.ty) * 0.5f) * rh - 0.5f - jy;

				// this view pixel moved as its traced sample did
				float hx = ox + (fx - sx) * to_view_x, hy = oy + (fy - sy) * to_view_y;
				bool here = false;

				// Something that moves with the eye, like the weapon in hand,
				// was not where the world says: it was on this same pixel. Take
				// that when the place it should have come from held something
				// else and this pixel held much the same thing.
				if (!sky && (std::fabs(fx - sx) > 0.01f || std::fabs(fy - sy) > 0.01f))
				{
					const int qx = (int)std::floor(fx + 0.5f), qy = (int)std::floor(fy + 0.5f);
					const bool inside = qx >= 0 && qy >= 0 && qx < rw && qy < rh;
					const bool same_there = inside && was.depth[(size_t)qy * rw + qx] >= 0.0f
						&& SameSurface(s->cur, was, i, (size_t)qy * rw + qx);
					const bool same_here = was.depth[i] > 0.0f
						&& std::fabs(was.depth[i] - s->cur.depth[i]) < 0.1f * s->cur.depth[i]
						&& Dot(s->cur.plane[i], was.plane[i]) > 0.8f;
					if (!same_there && same_here)
					{
						hx = (float)ox;
						hy = (float)oy;
						here = true;
					}
				}

				// Was it this point that was seen there, or something in
				// front of it? Where the weapon or a door has just moved
				// aside, what the picture held was the weapon or the door:
				// nothing to do with what shows now. Last frame's traced
				// picture knows how far off what it showed was, to within a
				// traced pixel of where.
				bool there = sky || here;
				if (!there)
				{
					const float expected = std::sqrt(Dot(v, v));
					const int qx = (int)std::floor(fx + 0.5f), qy = (int)std::floor(fy + 0.5f);
					for (int t = 0; t < 9 && !there; t++)
					{
						const int x = qx + t % 3 - 1, y = qy + t / 3 - 1;
						if (x < 0 || y < 0 || x >= rw || y >= rh)
							continue;
						const float held = was.depth[(size_t)y * rw + x];
						there = held >= 0.0f && std::fabs(held - expected) <= 0.1f * expected;
					}
				}

				if (there && hx >= 0.0f && hy >= 0.0f && hx <= vw - 1.0f && hy <= vh - 1.0f)
				{
					const int ix = std::min((int)hx, vw - 2 < 0 ? 0 : vw - 2), iy = std::min((int)hy, vh - 2 < 0 ? 0 : vh - 2);
					const int ix1 = std::min(ix + 1, vw - 1), iy1 = std::min(iy + 1, vh - 1);
					const float ax = hx - ix, ay = hy - iy;
					const size_t q00 = (size_t)iy * vw + ix, q01 = (size_t)iy * vw + ix1;
					const size_t q10 = (size_t)iy1 * vw + ix, q11 = (size_t)iy1 * vw + ix1;

					h = (prev[q00] * (1.0f - ax) + prev[q01] * ax) * (1.0f - ay)
						+ (prev[q10] * (1.0f - ax) + prev[q11] * ax) * ay;
					hcount = (prev_count[q00] * (1.0f - ax) + prev_count[q01] * ax) * (1.0f - ay)
						+ (prev_count[q10] * (1.0f - ax) + prev_count[q11] * ax) * ay;
					found = true;
				}
			}

			if (found && hcount > 0.0f)
			{
				// what the traced pixels around here show now bounds what history may say
				const Vec3 clamped = Max(s->near_lo[i], Min(s->near_hi[i], h));

				// history that had to be pulled back a long way was about
				// something else; let the picture catch up quickly
				const float pulled = Luminance(Max(clamped - h, h - clamped));
				if (pulled > 0.1f * (Luminance(clamped) + 0.02f))
					hcount = std::min(hcount, 4.0f);

				count = std::min(hcount + nearness, max_count);
				out = clamped + (c - clamped) * std::min(1.0f, nearness / count);
			}
		}

		s->steady[o] = out;
		s->steady_count[o] = count;
		if (out_row && t.x + ox >= t.x0 && t.x + ox < t.x1)
			out_row[t.x + ox] = ToneMap(out, grade);
	}
}

void RenderView(pt_backend_t *b, const pt_view_t *view)
{
	CpuBackend *s = Self(b);
	s->view = *view;
	s->has_view = true;

	int x0, y0, x1, y1;
	ClipView(s, x0, y0, x1, y1);
	if (x1 <= x0 || y1 <= y0)
		return;

	if (!s->world)
	{
		for (int y = y0; y < y1; y++)
			std::fill(s->scene.begin() + (size_t)y * s->width + x0, s->scene.begin() + (size_t)y * s->width + x1, 0u);
		s->stats[0] = 0;
		return;
	}

	const auto start = std::chrono::steady_clock::now();

	const float scale = view->scale < 0.05f ? 0.05f : (view->scale > 1.0f ? 1.0f : view->scale);
	const int rw = std::max(1, (int)(view->width * scale + 0.5f));
	const int rh = std::max(1, (int)(view->height * scale + 0.5f));
	const int samples = std::max(1, view->samples);
	const int bounces = std::max(0, view->bounces);
	const size_t count = (size_t)rw * rh;

	if (rw != s->rw || rh != s->rh)
	{
		s->rw = rw;
		s->rh = rh;
		s->cur.Resize(count);
		s->prev.Resize(count);
#ifdef PT_AVX2_KERNELS
		s->filter_geo.Resize(rw, rh, kGeoRows);
		s->filter_a.Resize(rw, rh, kLightRows);
		s->filter_b.Resize(rw, rh, kLightRows);
#else
		s->filter_geo.assign(count, FilterGeo());
		s->filter_a.assign(count, FilterLight());
		s->filter_b.assign(count, FilterLight());
#endif
		s->hdr.assign(count, Vec3());
		s->near_lo.assign(count, Vec3());
		s->near_hi.assign(count, Vec3());
		s->antialiased = false;
		s->ldr.assign(count, 0);
		s->have_history = false;
	}
	if (view->restart)
	{
		s->have_history = false;
		s->antialiased = false;
	}

	BuildFrame(s->frame, view->scene, s->textures);
	const auto built = std::chrono::steady_clock::now();

	Camera cam;
	cam.origin = Vec3(view->origin);
	cam.forward = Vec3(view->forward);
	cam.right = Vec3(view->right);
	cam.up = Vec3(view->up);
	cam.tx = std::tan(view->fov_x * kPi / 360.0f);
	cam.ty = std::tan(view->fov_y * kPi / 360.0f);

	Scene sc;
	sc.world = s->world.get();
	sc.frame = &s->frame;
	sc.light_styles = view->light_styles;
	sc.num_light_styles = view->light_styles ? view->num_light_styles : 0;
	sc.anim_frame = view->anim_frame < 0 ? 0 : view->anim_frame;
	sc.time = view->time;
	sc.handles = &s->textures;
	{
		const Vec3 axis(view->sky_axis);
		const float angle = view->sky_angle * (kPi / 180.0f);
		if (Dot(axis, axis) > 0.5f && angle != 0.0f)
		{
			sc.sky_axis = Normalize(axis);
			sc.sky_sin = std::sin(angle);
			sc.sky_cos = std::cos(angle);
		}
	}
	sc.light_samples = view->light_samples > 0 ? std::min(view->light_samples, 64) : 8;
	sc.max_sample = view->firefly_clamp > 0.0f ? view->firefly_clamp : 40.0f;
	sc.wave_strength = std::max(0.0f, view->wave_strength);
	sc.wave_reach = std::min(std::max(view->wave_reach, 0.0f), 8.0f);
	sc.water_shafts = std::min(std::max(view->water_shafts, 0.0f), 8.0f);
	sc.water_wet = std::min(std::max(view->water_wet, 0.0f), 1.0f);
	sc.swell = sc.wave_strength > 0.0f && s->world && !s->world->waters.empty();
	sc.filter_textures = view->texture_filter != 0;
	sc.reflections = view->reflections;
	sc.view_mode = view->view_mode;
	sc.metal_colour = std::max(0.0f, view->metal_colour);
	sc.reflection_bounces = std::max(1, view->reflection_bounces > 0 ? view->reflection_bounces : bounces);
	sc.reflection_rate = std::max(0.0f, view->reflection_rate);
	sc.refraction = view->refraction != 0;
	sc.fog_density = view->fog ? std::min(std::max(view->fog_density, 0.0f), 0.05f) : 0.0f;
	if (bounces < 1)
		sc.reflections = 0;		// no bounces at all means none off mirrors either
	s->moving_history = (float)std::min(std::max(view->history, 1), 512);
	s->fog_history = view->fog_history >= 1 ? std::min((float)view->fog_history, s->moving_history) : s->moving_history;
	sc.fog_samples = std::min(std::max(view->fog_samples, 1), 16);
	s->reflection_history = view->reflection_history != 0;
	const int passes = (view->debug || view->filter >= 2) ? std::min(std::max(view->denoise, 0), kMaxFilterPasses) : 0;
	s->pool.SetLimit(view->threads);
	s->frame_index++;

	// everything outside the camera that changes the picture
	uint32_t hash = s->frame.hash;
	hash = HashBytes(sc.light_styles, (size_t)sc.num_light_styles * sizeof(float), hash);
	hash = HashBytes(&sc.anim_frame, sizeof(sc.anim_frame), hash);
	hash = HashBytes(&bounces, sizeof(bounces), hash);
	{
		const float settings[] = {(float)samples, (float)sc.light_samples, sc.max_sample, sc.wave_strength,
			(float)sc.filter_textures, (float)sc.reflections, (float)sc.reflection_bounces, sc.reflection_rate,
			(float)sc.refraction, view->exposure, sc.fog_density, (float)sc.view_mode, sc.metal_colour,
			(float)sc.fog_samples, sc.water_shafts, sc.water_wet};
		hash = HashBytes(settings, sizeof(settings), hash);
	}
	if (s->world->has_waves)
		hash = HashBytes(&sc.time, sizeof(sc.time), hash);
	hash = HashBytes(&sc.sky_sin, sizeof(sc.sky_sin), hash);

	// each frame looks through a slightly different point of every pixel, so
	// that over time edges are seen from all across it
	// What is done about noise: 2, all there is; 1, nothing, but frames add up
	// while the eye is at rest; 0, nothing. Without the first there is nothing
	// of an earlier view in the picture, ever. A debug view is shown filtered.
	const int filtering = view->debug ? 2 : std::min(std::max(view->filter, 0), 2);
	const bool same_camera = s->have_history && cam == s->prev_camera;
	const bool had_history = s->have_history;
	s->have_history = had_history && (filtering == 2 || (filtering == 1 && same_camera));
	s->filtering = filtering;
	// (only for the filtered picture, whose last pass puts it back together
	// from those points: a raw one would shake by a part of a pixel)
	const bool antialias = view->antialias != 0 && filtering == 2;
	const float jx = antialias ? Halton(s->frame_index % 16 + 1, 2) - 0.5f : 0.0f;
	const float jy = antialias ? Halton(s->frame_index % 16 + 1, 3) - 0.5f : 0.0f;
	s->jitter_x = jx;
	s->jitter_y = jy;

	s->pool.Run(rh, [&](int y)
	{
		for (int x = 0; x < rw; x++)
			TracePixel(s, sc, cam, jx, jy, x, y, samples, bounces);
	});
	const auto traced = std::chrono::steady_clock::now();

	// with nothing changing the average may run forever and converge
	// (or, adding frames up at rest, whenever the eye has not moved: what
	// does move in the view starts afresh by itself, see Accumulate)
	const bool still = s->have_history && cam == s->prev_camera && (filtering == 1 || hash == s->prev_hash);
	const float max_history = still ? 65536.0f : s->moving_history;
	const Camera prev_cam = s->prev_camera;
	s->pool.Run(rh, [&](int y) { Accumulate(s, prev_cam, max_history, y); });
	const auto accumulated = std::chrono::steady_clock::now();

	// gather what the filter needs, then let it ping-pong between two buffers
#ifdef PT_AVX2_KERNELS
	s->pool.Run(rh, [&](int y)
	{
		const size_t gs = s->filter_geo.stride, ls = s->filter_a.stride;
		float *g = s->filter_geo.Row(y), *l = s->filter_a.Row(y);
		for (int x = 0; x < rw; x++)
		{
			const size_t i = (size_t)y * rw + x;
			g[kGeoPosX * gs + x] = s->cur.pos[i].x;
			g[kGeoPosY * gs + x] = s->cur.pos[i].y;
			g[kGeoPosZ * gs + x] = s->cur.pos[i].z;
			g[kGeoDepth * gs + x] = s->cur.depth[i];
			g[kGeoNormalX * gs + x] = s->cur.normal[i].x;
			g[kGeoNormalY * gs + x] = s->cur.normal[i].y;
			g[kGeoNormalZ * gs + x] = s->cur.normal[i].z;
			g[kGeoRoughness * gs + x] = s->cur.roughness[i];
			g[kGeoPlaneX * gs + x] = s->cur.plane[i].x;
			g[kGeoPlaneY * gs + x] = s->cur.plane[i].y;
			g[kGeoPlaneZ * gs + x] = s->cur.plane[i].z;
			for (int c = 0; c < kChannels; c++)
			{
				float *to = l + 4 * c * ls + x;
				to[0] = s->cur.light[c][i].x;
				to[ls] = s->cur.light[c][i].y;
				to[2 * ls] = s->cur.light[c][i].z;
				to[3 * ls] = s->cur.variance[c][i];
			}
		}
	});

	const FilterRows *in = &s->filter_a;
	for (int pass = 0; pass < passes; pass++)
	{
		FilterRows *out = (pass & 1) ? &s->filter_a : &s->filter_b;
		s->pool.Run(rh, [&](int y) { FilterRow(rw, rh, s->filter_geo, *in, *out, 1 << pass, y); });
		in = out;
	}
	// the light the filter leaves in a channel of a pixel, and the noise
	const auto lit = [&](int x, int y, int ch)
	{
		const float *l = in->Row(y) + 4 * ch * in->stride + x;
		return Vec3(l[0], l[in->stride], l[2 * in->stride]);
	};
	const auto noise = [&](int x, int y, int ch)
	{
		return std::sqrt(in->Row(y)[4 * ch * in->stride + 3 * in->stride + x]);
	};
#else
	s->pool.Run(rh, [&](int y)
	{
		for (int x = 0; x < rw; x++)
		{
			const size_t i = (size_t)y * rw + x;
			FilterGeo &g = s->filter_geo[i];
			g.pos = s->cur.pos[i];
			g.depth = s->cur.depth[i];
			g.normal = s->cur.normal[i];
			g.roughness = s->cur.roughness[i];
			g.plane = s->cur.plane[i];
			FilterLight &l = s->filter_a[i];
			for (int c = 0; c < kChannels; c++)
			{
				l.r[c] = s->cur.light[c][i].x;
				l.g[c] = s->cur.light[c][i].y;
				l.b[c] = s->cur.light[c][i].z;
				l.var[c] = s->cur.variance[c][i];
			}
		}
	});

	const FilterLight *in = s->filter_a.data();
	for (int pass = 0; pass < passes; pass++)
	{
		FilterLight *out = (pass & 1) ? s->filter_a.data() : s->filter_b.data();
		const FilterGeo *geo = s->filter_geo.data();
		s->pool.Run(rh, [&](int y) { FilterRow(rw, rh, geo, in, out, 1 << pass, y); });
		in = out;
	}
	// the light the filter leaves in a channel of a pixel, and the noise
	const auto lit = [&](int x, int y, int ch)
	{
		const FilterLight &l = in[(size_t)y * rw + x];
		return Vec3(l.r[ch], l.g[ch], l.b[ch]);
	};
	const auto noise = [&](int x, int y, int ch) { return std::sqrt(in[(size_t)y * rw + x].var[ch]); };
#endif
	const auto filtered = std::chrono::steady_clock::now();

	// Exposure: the user's setting times, if wanted, whatever brings this
	// scene's typical brightness to a fixed level.
	const int debug = view->debug;
	const float exposure = debug ? 1.0f : view->exposure * (view->auto_exposure ? s->auto_exposure : 1.0f);
	s->pool.Run(rh, [&](int y)
	{
		for (int x = 0; x < rw; x++)
		{
			const size_t i = (size_t)y * rw + x;
			Vec3 c = s->cur.add[i];
			for (int ch = 0; ch < kChannels; ch++)
				c += s->cur.albedo[ch][i] * lit(x, y, ch);

			// one part of the picture on its own, for finding where a fault lies
			switch (debug)
			{
			case 1: c = s->cur.albedo[kDiffuse][i]; break;
			case 2: c = lit(x, y, kDiffuse); break;
			case 3: c = s->cur.albedo[kSpecular][i] * lit(x, y, kSpecular); break;
			case 4: c = s->cur.albedo[kOver][i] * lit(x, y, kOver); break;
			case 5: c = s->cur.add[i]; break;
			case 6: c = s->cur.normal[i] * 0.25f + Vec3(0.25f); break;
			case 7: c = Vec3(std::min(s->cur.length[i], 32.0f) / 64.0f); break;
			case 8: c = Vec3(std::min(s->cur.over_length[i], 32.0f) / 64.0f); break;
			case 9: c = Vec3(s->cur.bent[i] ? 0.5f : 0.05f); break;
			case 10: c = Vec3(s->cur.depth[i] * 0.002f); break;
			case 11: c = lit(x, y, kFog); break;
			case 12: c = Vec3(noise(x, y, kDiffuse) * 4.0f); break;
			default: break;
			}
			if (sc.view_mode == PT_VIEW_BOUNCES)
				c = BounceColour(lit(x, y, kDiffuse).x);
			else if (sc.view_mode == PT_VIEW_COST)
				c = CostColour(lit(x, y, kDiffuse).x);
			s->hdr[i] = c * exposure;
		}
	});

	float typical = 0.0f;
	if (!debug)
	{
		// The eye adapts: measured on what was just made, used from the next
		// frame on, and followed over about a second so it does not pump.
		typical = TypicalLuminance(s) / exposure;
		if (typical > 0.0f)
		{
			const float want = std::min(16.0f, std::max(0.125f, kTypicalTarget / typical));
			const float dt = view->time - s->prev_time;
			if (!s->have_exposure || dt < 0.0f || dt > 1.0f)
				s->auto_exposure = want;
			else
				s->auto_exposure += (want - s->auto_exposure) * (1.0f - std::exp(-dt * 2.5f));
			s->have_exposure = true;
		}
		if (view->bloom > 0.0f)
			Bloom(s, view->bloom, view->bloom_max);
	}
	s->prev_time = view->time;

	Grade grade;
	grade.curve = view->tonemap;
	grade.saturation = std::max(0.0f, view->saturation);
	grade.contrast = view->contrast > 0.0f ? view->contrast : 1.0f;

	Target target;
	target.x = view->x;
	target.y = view->y;
	target.width = view->width;
	target.height = view->height;
	target.x0 = x0;
	target.y0 = y0;
	target.x1 = x1;
	target.y1 = y1;

	const int vw = view->width, vh = view->height;
	if (s->out_w != vw || s->out_h != vh)
	{
		s->out_w = vw;
		s->out_h = vh;
		const size_t out_count = (size_t)vw * vh;
		s->steady.assign(out_count, Vec3());
		s->steady_prev.assign(out_count, Vec3());
		s->steady_count.assign(out_count, 0.0f);
		s->steady_count_prev.assign(out_count, 0.0f);
		s->antialiased = false;
	}

	const bool resolve_history = s->have_history && antialias && s->antialiased && !debug && filtering == 2;
	s->antialiased = antialias;
	if (antialias)
	{
		// the range of colours around each traced pixel, which bounds its history
		s->pool.Run(rh, [&](int y)
		{
			for (int x = 0; x < rw; x++)
			{
				Vec3 lo = s->hdr[(size_t)y * rw + x], hi = lo;
				for (int dy = -1; dy <= 1; dy++)
				{
					const int qy = y + dy;
					if (qy < 0 || qy >= rh)
						continue;
					for (int dx = -1; dx <= 1; dx++)
					{
						const int qx = x + dx;
						if (qx < 0 || qx >= rw)
							continue;
						const Vec3 q = s->hdr[(size_t)qy * rw + qx];
						lo = Min(lo, q);
						hi = Max(hi, q);
					}
				}
				s->near_lo[(size_t)y * rw + x] = lo;
				s->near_hi[(size_t)y * rw + x] = hi;
			}
		});

		const float max_count = cam == prev_cam ? 1024.0f : 16.0f;
		s->pool.Run(vh, [&](int oy) { ResolveRow(s, cam, prev_cam, resolve_history, max_count, target, grade, oy); });
	}
	else
		s->pool.Run(vh, [&](int oy) { UpscaleRow(s, target, grade, oy); });

	// this frame becomes the history the next one looks back at
	std::swap(s->cur, s->prev);
	std::swap(s->steady, s->steady_prev);
	std::swap(s->steady_count, s->steady_count_prev);
	s->prev_camera = cam;
	s->prev_hash = hash;
	s->have_history = true;

	const auto end = std::chrono::steady_clock::now();
	const auto ms = [](std::chrono::steady_clock::time_point a, std::chrono::steady_clock::time_point c)
	{
		return std::chrono::duration<double, std::milli>(c - a).count();
	};
	snprintf(s->stats, sizeof(s->stats), "%dx%d %dspp %db: %.1f ms|build %.1f trace %.1f history %.1f filter %.1f out %.1f|%zu dyn tris typ %.4f exp %.2f%s",
		rw, rh, samples, bounces, ms(start, end), ms(start, built), ms(built, traced), ms(traced, accumulated),
		ms(accumulated, filtered), ms(filtered, end),
		s->frame.tris.size(), typical, exposure, still ? " still" : "");

	s->stage_ms[0] = (float)ms(start, built);
	s->stage_ms[1] = (float)ms(built, traced);
	s->stage_ms[2] = (float)ms(traced, accumulated);
	s->stage_ms[3] = (float)ms(accumulated, filtered);
	s->stage_ms[4] = (float)ms(filtered, end);
	s->stages_new = true;
}

void Present(pt_backend_t *b, const uint32_t *overlay, const pt_rect_t *, int)
{
	CpuBackend *s = Self(b);

	int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
	if (s->has_view)
		ClipView(s, x0, y0, x1, y1);

	for (int y = 0; y < s->height; y++)
	{
		const uint32_t *ov = &overlay[(size_t)y * s->width];
		const uint32_t *sc = &s->scene[(size_t)y * s->width];
		uint32_t *out = &s->dibbits[(size_t)y * s->width];
		const bool rowin = s->has_view && y >= y0 && y < y1;

		for (int x = 0; x < s->width; x++)
		{
			const uint32_t o = ov[x];
			const uint32_t a = o >> 24;
			const uint32_t bg = (rowin && x >= x0 && x < x1) ? sc[x] : 0;
			// overlay is R,G,B,A bytes; the DIB wants 0x00RRGGBB
			const uint32_t orgb = ((o & 0xff) << 16) | (o & 0xff00) | ((o >> 16) & 0xff);

			if (a == 255)
				out[x] = orgb;
			else if (a == 0)
				out[x] = bg;
			else
			{
				const uint32_t ia = 255 - a;
				const uint32_t rb = (((bg & 0xff00ff) * ia + 0x800080) >> 8) & 0xff00ff;
				const uint32_t g = (((bg & 0x00ff00) * ia + 0x008000) >> 8) & 0x00ff00;
				out[x] = orgb + (rb | g);
			}
		}
	}
	s->shown[0] = x0; s->shown[1] = y0; s->shown[2] = x1; s->shown[3] = y1;
	s->has_view = false;

#ifdef _WIN32
	HDC dc = GetDC(s->hwnd);
	if (dc)
	{
		BitBlt(dc, 0, 0, s->width, s->height, s->memdc, 0, 0, SRCCOPY);
		ReleaseDC(s->hwnd, dc);
	}
#else
	XPutImage(s->display, s->window, s->gc, s->image, 0, 0, 0, 0, s->width, s->height);
	XFlush(s->display);
#endif
}

int ReadPixels(pt_backend_t *b, uint32_t *pixels, int with_overlay)
{
	CpuBackend *s = Self(b);

	// a view made since the last frame was shown can be read already
	if (s->has_view)
		ClipView(s, s->shown[0], s->shown[1], s->shown[2], s->shown[3]);

	for (int y = 0; y < s->height; y++)
	{
		const uint32_t *in = with_overlay ? &s->dibbits[(size_t)y * s->width] : &s->scene[(size_t)y * s->width];
		uint32_t *out = &pixels[(size_t)y * s->width];
		const bool rowin = with_overlay || (y >= s->shown[1] && y < s->shown[3]);

		for (int x = 0; x < s->width; x++)
		{
			const uint32_t c = (rowin && (with_overlay || (x >= s->shown[0] && x < s->shown[2]))) ? in[x] : 0;
			out[x] = ((c >> 16) & 0xff) | (c & 0xff00) | ((c & 0xff) << 16) | 0xff000000u;
		}
	}
	return 1;
}

const char *Stats(pt_backend_t *b)
{
	return Self(b)->stats;
}

int Stages(pt_backend_t *b, pt_stage_t *stages, int max)
{
	CpuBackend *s = Self(b);
	if (!s->stages_new)
		return 0;
	s->stages_new = false;

	const int count = std::min(max, kNumStages);
	for (int i = 0; i < count; i++)
	{
		stages[i].name = kStageNames[i];
		stages[i].ms = s->stage_ms[i];
	}
	return count;
}

#ifdef PT_PERF
// the same, every time it is asked, as work done on the processor
int Perf(pt_backend_t *b, pt_stage_t *stages, int max)
{
	CpuBackend *s = Self(b);
	const int count = std::min(max, kNumStages);
	for (int i = 0; i < count; i++)
	{
		stages[i].name = kStageNames[i];
		stages[i].ms = s->stage_ms[i];
		stages[i].where = 0;
	}
	return count;
}
#endif

} // namespace

// built twice into one program, each build is made under a name of its own
// and pt_cpu_create picks between them: see pt_cpu_pick.cpp
#ifndef PT_CPU_CREATE
#define PT_CPU_CREATE pt_cpu_create
#endif

extern "C" pt_backend_t *PT_CPU_CREATE(const pt_create_t *ci, char *err, int errlen)
{
	InitTables();

	CpuBackend *s = new CpuBackend;
	s->base.name = "CPU path tracer";
	s->base.destroy = Destroy;
	s->base.load_world = LoadWorld;
	s->base.texture_create = TextureCreate;
	s->base.texture_destroy = TextureDestroy;
	s->base.texture_update = TextureUpdate;
	s->base.render_view = RenderView;
	s->base.present = Present;
	s->base.stats = Stats;
	s->base.stages = Stages;
#ifdef PT_PERF
	s->base.perf = Perf;
#else
	s->base.perf = nullptr;
#endif
	s->base.read_pixels = ReadPixels;
	NameDevice(s->device, sizeof(s->device), s->pool.Threads());
	s->base.device = s->device;
	s->log = ci->log;
	s->width = ci->width;
	s->height = ci->height;
	s->scene.assign((size_t)ci->width * ci->height, 0);

#ifdef _WIN32
	s->hwnd = (HWND)ci->hwnd;
	BITMAPINFO bmi{};
	bmi.bmiHeader.biSize = sizeof(bmi.bmiHeader);
	bmi.bmiHeader.biWidth = ci->width;
	bmi.bmiHeader.biHeight = -ci->height;	// top-down
	bmi.bmiHeader.biPlanes = 1;
	bmi.bmiHeader.biBitCount = 32;
	bmi.bmiHeader.biCompression = BI_RGB;

	void *bits = nullptr;
	s->memdc = CreateCompatibleDC(nullptr);
	if (s->memdc)
		s->dib = CreateDIBSection(s->memdc, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
	if (!s->memdc || !s->dib || !bits)
	{
		snprintf(err, errlen, "could not create a %dx%d framebuffer", ci->width, ci->height);
		Destroy(&s->base);
		return nullptr;
	}
	s->dibbits = (uint32_t *)bits;
	s->olddib = SelectObject(s->memdc, s->dib);
	const char *const presentation = "GDI";
#else
	// the picture is 0x00RRGGBB, which is what a 24 bit true colour X visual
	// on a little endian machine takes as it is
	s->display = (Display *)ci->hinstance;
	s->window = (Window)(uintptr_t)ci->hwnd;
	XWindowAttributes wa;
	if (!s->display || !XGetWindowAttributes(s->display, s->window, &wa) || wa.depth != 24
		|| wa.visual->red_mask != 0xff0000 || wa.visual->blue_mask != 0xff)
	{
		snprintf(err, errlen, "the window is not 24 bit true colour");
		Destroy(&s->base);
		return nullptr;
	}
	s->dibbits = (uint32_t *)calloc((size_t)ci->width * ci->height, sizeof(uint32_t));
	s->gc = XCreateGC(s->display, s->window, 0, nullptr);
	s->image = XCreateImage(s->display, wa.visual, 24, ZPixmap, 0, (char *)s->dibbits,
		ci->width, ci->height, 32, ci->width * 4);
	if (!s->dibbits || !s->gc || !s->image)
	{
		snprintf(err, errlen, "could not create a %dx%d framebuffer", ci->width, ci->height);
		Destroy(&s->base);
		return nullptr;
	}
	const char *const presentation = "X11";
#endif

	if (ci->log)
	{
		char msg[128];
		snprintf(msg, sizeof(msg), "CPU path tracer: %d threads, %s presentation\n", s->pool.Threads(), presentation);
		ci->log(msg);
	}
	return &s->base;
}
