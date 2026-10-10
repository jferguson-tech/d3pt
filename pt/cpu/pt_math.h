// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Jonathan Ferguson
#pragma once

#include "pt_ns.h"

#include <cmath>
#include <cstdint>

namespace PT_NS {

const float kPi = 3.14159265358979323846f;
const float kInvPi = 1.0f / kPi;

struct Vec3
{
	float x, y, z;

	Vec3() : x(0), y(0), z(0) {}
	Vec3(float x_, float y_, float z_) : x(x_), y(y_), z(z_) {}
	explicit Vec3(float s) : x(s), y(s), z(s) {}
	explicit Vec3(const float *p) : x(p[0]), y(p[1]), z(p[2]) {}

	float operator[](int i) const { return (&x)[i]; }
	float &operator[](int i) { return (&x)[i]; }
};

inline Vec3 operator+(Vec3 a, Vec3 b) { return Vec3(a.x + b.x, a.y + b.y, a.z + b.z); }
inline Vec3 operator-(Vec3 a, Vec3 b) { return Vec3(a.x - b.x, a.y - b.y, a.z - b.z); }
inline Vec3 operator*(Vec3 a, Vec3 b) { return Vec3(a.x * b.x, a.y * b.y, a.z * b.z); }
inline Vec3 operator*(Vec3 a, float s) { return Vec3(a.x * s, a.y * s, a.z * s); }
inline Vec3 operator*(float s, Vec3 a) { return Vec3(a.x * s, a.y * s, a.z * s); }
inline Vec3 operator/(Vec3 a, float s) { const float i = 1.0f / s; return Vec3(a.x * i, a.y * i, a.z * i); }
inline Vec3 operator-(Vec3 a) { return Vec3(-a.x, -a.y, -a.z); }
inline Vec3 &operator+=(Vec3 &a, Vec3 b) { a.x += b.x; a.y += b.y; a.z += b.z; return a; }
inline Vec3 &operator*=(Vec3 &a, Vec3 b) { a.x *= b.x; a.y *= b.y; a.z *= b.z; return a; }
inline Vec3 &operator*=(Vec3 &a, float s) { a.x *= s; a.y *= s; a.z *= s; return a; }

inline float Dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Vec3 Cross(Vec3 a, Vec3 b)
{
	return Vec3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x);
}
inline float Length(Vec3 a) { return std::sqrt(Dot(a, a)); }
inline Vec3 Normalize(Vec3 a)
{
	const float l = Length(a);
	return l > 0.0f ? a / l : Vec3(0, 0, 1);
}
inline Vec3 Min(Vec3 a, Vec3 b) { return Vec3(a.x < b.x ? a.x : b.x, a.y < b.y ? a.y : b.y, a.z < b.z ? a.z : b.z); }
inline Vec3 Max(Vec3 a, Vec3 b) { return Vec3(a.x > b.x ? a.x : b.x, a.y > b.y ? a.y : b.y, a.z > b.z ? a.z : b.z); }
inline float MaxComponent(Vec3 a) { return a.x > a.y ? (a.x > a.z ? a.x : a.z) : (a.y > a.z ? a.y : a.z); }
inline float Luminance(Vec3 c) { return 0.2126f * c.x + 0.7152f * c.y + 0.0722f * c.z; }

// any two unit vectors perpendicular to n and to each other
inline void Basis(Vec3 n, Vec3 &t, Vec3 &b)
{
	const float s = n.z >= 0.0f ? 1.0f : -1.0f;
	const float a = -1.0f / (s + n.z);
	const float c = n.x * n.y * a;
	t = Vec3(1.0f + s * n.x * n.x * a, s * c, -s * n.x);
	b = Vec3(c, s + n.y * n.y * a, -n.y);
}

// small fast generator, one per path
struct Rng
{
	uint32_t state;
	uint32_t rays = 0;		// traced by whoever holds this, for PT_VIEW_COST

	explicit Rng(uint32_t seed) : state(seed) {}

	uint32_t Next()
	{
		// PCG hash step
		state = state * 747796405u + 2891336453u;
		uint32_t w = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
		return (w >> 22u) ^ w;
	}
	// [0, 1)
	float Float() { return (float)(Next() >> 8) * (1.0f / 16777216.0f); }
};

inline uint32_t Hash(uint32_t a, uint32_t b)
{
	uint32_t h = a * 0x9E3779B1u ^ (b + 0x7F4A7C15u + (a << 6) + (a >> 2));
	h ^= h >> 16; h *= 0x85EBCA6Bu;
	h ^= h >> 13; h *= 0xC2B2AE35u;
	h ^= h >> 16;
	return h;
}

} // namespace PT_NS
