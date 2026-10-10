// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Jonathan Ferguson
//
// Light transport: surfaces, their reflectance, and paths through the scene.
#pragma once

#include "pt_world.h"

namespace PT_NS {

const float kRayOffset = 0.03f;			// keeps rays off the surface they leave

// Below this roughness the specular lobe is too narrow for light sampling
// to find; lights are then picked up when a reflected ray happens to hit them.
const float kLightSampledRoughness = 0.25f;

// A point on a surface, ready to be lit
struct Surface
{
	const Tri		*tri;
	const Material	*mat;		// after animation
	Vec3			p;
	Vec3			ng;			// geometric normal, on the side the ray came from
	Vec3			n;			// shading normal
	Vec3			wo;			// unit, towards where the ray came from
	bool			front;		// seen from the triangle's counter clockwise side
	Vec3			colour;		// the texture's colour here
	Vec3			glow;		// the emission map's, where the material has one
	Vec3			kd;			// diffuse reflectance
	Vec3			f0;			// specular reflectance head on
	float			roughness;
	float			metallic;	// here: the material's, or its texture's
	float			alpha;		// GGX width, roughness squared
	bool			light_sampled_spec;
	bool			medium;		// not a surface at all but a point in the air: no facing, scatters evenly
	float			foam = 0.0f;	// how much of a liquid's surface froth covers here; it is in colour and cover already
	float			cover = 1.0f;	// how much of what is behind it the surface hides: the material's alpha, and the froth's

	// how much the specular lobe reflects in total towards wo, roughly
	Vec3 SpecularAlbedo() const;
};

// light from a sampled light, split by the lobe that reflects it
struct Lit
{
	Vec3	diffuse;	// irradiance: multiply by kd / pi
	Vec3	specular;	// radiance, already through the specular lobe
};

bool Finite(float f);
// limits how bright one sample may be, which tames fireflies
Vec3 ClampSample(Vec3 c, float max_luminance);

// See-through surfaces are one sided, as the game draws them: from behind
// they are not there. A pane of glass modelled as a slab would otherwise tint
// and reflect twice.
bool BackOfGlass(const Tri &tri, Vec3 dir);

// Nearest surface along the ray, in the world or the frame. Holes are stepped
// through, and so are surfaces the camera must not see when the ray comes
// from it. With cross set, surfaces that let light through are crossed at
// random in proportion to how much they pass. ray.tmin moves past what was
// skipped. held says what is done with the frame's triangles that the eye
// carries (PT_MAT_HELD): taken like any other, passed by, or the only ones
// met, for a ray that is cast at them alone. waves: a simulated liquid is met
// where its waves stand, as the eye must see it; without, it is the level
// sheet the map has for it, which is all that light finding its way about
// needs and costs every such ray less to look for.
enum HeldRays { kHeldToo, kNotHeld, kHeldOnly };
bool Closest(const Scene &sc, Ray &ray, Rng &rng, bool camera, bool cross, Hit &hit, const Tri *&tri,
	HeldRays held = kHeldToo, bool waves = false);

// smooth: filter the textures, for surfaces the eye sees directly. Further
// along a path the nearest texel is as good and cheaper.
// froth: whether to look for it on a simulated liquid, as only the eye's own rays do
void MakeSurface(const Scene &sc, const Tri &tri, const Hit &hit, const Ray &ray, Surface &s, bool smooth = false, bool froth = false);
// in this view mode the material is solid whatever its alpha says. Clay: all
// that is solid, and liquids. The white furnace: everything.
bool ViewSolid(int mode, const Material &mat);
// the lighting only view: the surface as it would be were its texture white
void WhiteSurface(Surface &s);
// what a view of one thing known of the surface shows (PT_VIEW_BASE_COLOUR
// to PT_VIEW_GLOW)
Vec3 SurfaceChannel(int mode, const Surface &s);
// the colour that stands for a number of bounces, see PT_VIEW_BOUNCES
Vec3 BounceColour(float bounces);
// and for a number of rays, see PT_VIEW_COST
Vec3 CostColour(float rays);

// what an emitter sends back along the ray that hit it
Vec3 Emitted(const Surface &s, bool seen);

// one of the world's lights, chosen by resampling
Lit DirectWorld(const Scene &sc, const Surface &s, Rng &rng, bool first_hit);
// The frame's lights. One chosen from them all; or all of its point lights,
// which is exact, and with it one of its balls of light, which is not: a
// point is drawn on the ball, and what comes of it is as noisy as the rest.
Lit DirectFrameOne(const Scene &sc, const Surface &s, Rng &rng);
Lit DirectFrameAll(const Scene &sc, const Surface &s, Rng &rng);
// How much the frame's lights put on a pixel, with no noise in it: what the
// point lights put on the surface (flash_light, as lit), what the balls
// would with nothing in the way, and what all of them light the air in front
// of it by, along the view ray to reach. Compared from frame to frame, it
// says where light has changed and by how much.
float FrameLightLevel(const Scene &sc, const Surface &s, Vec3 flash_light, Vec3 eye, Vec3 dir, float reach);
Lit DirectFrameBall(const Scene &sc, const Surface &s, Rng &rng);

// light arriving at a point in the air from one sampled light and the sky,
// as irradiance on a surface facing it
Vec3 DirectMedium(const Scene &sc, Vec3 p, Rng &rng);
void AddWater(const Scene &sc, Surface &s, bool under);

Vec3 SampleDiffuse(const Surface &s, Rng &rng);
// returns false if the sample is unusable; weight is what the lobe reflects of it
bool SampleSpecular(const Surface &s, Rng &rng, Vec3 &wi, Vec3 &weight);

// Radiance arriving back along the ray. camera: the ray left the eye.
// count_emitters: emitters in the light lists count if hit (the bounce that
// made this ray could not have sampled them). depth: bounces already taken.
// reached, if given, gets how far the ray went before it met anything;
// followed, if given, is raised by the number of rays the path was made of.
Vec3 Radiance(const Scene &sc, Ray ray, Rng &rng, bool camera, bool count_emitters, int depth, int max_bounces,
	float *reached = nullptr, int *followed = nullptr);

} // namespace PT_NS
