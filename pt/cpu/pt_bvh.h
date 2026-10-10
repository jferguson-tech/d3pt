// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Jonathan Ferguson
#pragma once

#include "pt_math.h"

#include <cfloat>
#include <immintrin.h>
#include <vector>

namespace PT_NS {

struct Ray
{
	Vec3	o, d;
	float	tmin, tmax;
};

struct Hit
{
	float		t, u, v;	// u, v: barycentric weights of vertices 1 and 2
	uint32_t	tri;		// index as given to Build
};

// Bounding volume hierarchy over a triangle soup. Each node has up to four
// children and holds their boxes, so one SSE test decides which of them a
// ray enters, and a ray passes through half as many nodes as with two.
class Bvh
{
public:
	// What Intersect8 and AnyHit8 are told of a triangle, so as not to have to
	// ask about the plain ones, which are nearly all of them.
	enum
	{
		kAsk = 1,		// what it does to a ray is for the caller to say
		kChancy = 2		// and that may be one thing or another by chance; with kAsk
	};

	// verts holds 3 positions per triangle. marks, if given, has a byte per
	// triangle of the bits above.
	void Build(const Vec3 *verts, uint32_t num_tris, const uint8_t *marks = nullptr);

	bool Empty() const { return tris_.empty(); }

	// closest hit in (tmin, tmax)
	bool Intersect(const Ray &ray, Hit &hit) const
	{
		return IntersectIf(ray, hit, [](uint32_t, float, float) { return true; });
	}

	// closest hit among the triangles for which accept(tri, u, v) says yes.
	// Unlike restarting the ray past a rejected triangle, this cannot step
	// over another triangle lying in the same plane.
	template <class F>
	bool IntersectIf(const Ray &ray, Hit &hit, F accept) const;

	// true if blocks(tri, u, v) says yes for any triangle in (tmin, tmax)
	template <class F>
	bool AnyHit(const Ray &ray, F blocks) const;

#ifdef PT_AVX2_KERNELS
	// The same two questions put to a second tree over the same triangles,
	// with eight children to a node and up to eight triangles to a leaf, so
	// that one AVX2 test deals with a whole node or a whole leaf. The answers
	// are IntersectIf's and AnyHit's to the last bit. A triangle is tested
	// with the same arithmetic, so what is left to differ is which triangles
	// get tested at all, and that the two trees decide each in its own way
	// in three cases. These recognise the three and say they are unsure
	// instead of answering, and the question goes to the others:
	//
	//	- Two triangles at the same distance, or as good as: the one tested
	//	  first wins, or gets the other skipped as being no nearer.
	//	- A hit within a hair of the triangle's edge. A ray that close to the
	//	  edge of a box may be taken to miss it, and the boxes differ.
	//	- For AnyHit, a triangle the ray gets past by chance: the chances are
	//	  drawn in the order such triangles are met.

	// As IntersectIf, but accept is asked only of the triangles marked
	// kChancy: the rest are taken as accepted. unsure: it may not be what
	// IntersectIf would find.
	template <class F>
	bool Intersect8(const Ray &ray, Hit &hit, F accept, bool &unsure) const;

	// kind(tri, u, v) says what a triangle in (tmin, tmax) does to the ray:
	// 0 lets it by, 1 stops it, 2 does one or the other by chance. It is
	// asked only of the triangles marked kAsk, the rest being taken to stop
	// it, and only one marked kChancy may be of the third kind. Returns 0 if
	// nothing stops the ray, 1 if something does, and 2 if unsure.
	template <class F>
	int AnyHit8(const Ray &ray, F kind) const;
#endif

private:
	static const uint32_t kNone = ~0u;
	struct Node
	{
		// the children's boxes, one child per lane. An unused lane has a
		// box at infinity that no ray reaches.
		float		lox[4], loy[4], loz[4], hix[4], hiy[4], hiz[4];
		uint32_t	child[4];	// inner: node index. leaf: first triangle. unused: kNone
		uint32_t	count[4];	// triangles in a leaf, 0 for an inner node
	};
	struct Node2;				// what the builder makes first: two children to a node
	struct Tri
	{
		Vec3		p0, e1, e2;
		uint32_t	index;
	};
	struct BuildPrim;
	struct Ref
	{
		uint32_t	index, count;
	};

	Ref BuildNode(std::vector<Node2> &out, BuildPrim *prims, uint32_t first, uint32_t count, float *lo, float *hi);
	uint32_t Collapse(const std::vector<Node2> &src, uint32_t index);

	// which children the ray enters before tmax, as a 4 bit mask, and where
	struct RayPack
	{
		__m128	ox, oy, oz, ix, iy, iz, tmin;

		explicit RayPack(const Ray &r)
			: ox(_mm_set1_ps(r.o.x)), oy(_mm_set1_ps(r.o.y)), oz(_mm_set1_ps(r.o.z)),
			  ix(_mm_set1_ps(1.0f / r.d.x)), iy(_mm_set1_ps(1.0f / r.d.y)), iz(_mm_set1_ps(1.0f / r.d.z)),
			  tmin(_mm_set1_ps(r.tmin)) {}
	};
	static int HitChildren(const Node &n, const RayPack &r, float tmax, float *tnear);
	static bool HitTri(const Tri &tri, const Ray &ray, float tmax, float &t, float &u, float &v);

	std::vector<Node>	nodes_;		// nodes_[0] is the root
	std::vector<Tri>	tris_;

#ifdef PT_AVX2_KERNELS
	static const int kStack8 = 512;
	struct Node8
	{
		// as Node, with eight lanes
		float		lox[8], loy[8], loz[8], hix[8], hiy[8], hiz[8];
		uint32_t	child[8];	// inner: node index. leaf: its Tri8. unused: kNone
		uint8_t		count[8];	// triangles in a leaf, 0 for an inner node
		uint8_t		chancy;		// a bit per child: a triangle marked kChancy is somewhere under it
		uint8_t		pad[7];
	};
	// the triangles of one leaf, a lane each
	struct Tri8
	{
		float		p0x[8], p0y[8], p0z[8], e1x[8], e1y[8], e1z[8], e2x[8], e2y[8], e2z[8];
		uint32_t	index[8];
		uint8_t		ask, chancy;	// a bit per lane: marked kAsk, marked kChancy
		uint8_t		pad[6];
	};
	struct Span
	{
		uint32_t	first, total;
	};
	struct RayPack8
	{
		__m256	ox, oy, oz, ix, iy, iz, tmin;

		explicit RayPack8(const Ray &r)
			: ox(_mm256_set1_ps(r.o.x)), oy(_mm256_set1_ps(r.o.y)), oz(_mm256_set1_ps(r.o.z)),
			  ix(_mm256_set1_ps(1.0f / r.d.x)), iy(_mm256_set1_ps(1.0f / r.d.y)), iz(_mm256_set1_ps(1.0f / r.d.z)),
			  tmin(_mm256_set1_ps(r.tmin)) {}
	};

	void Build8(const std::vector<Node2> &src, const uint8_t *marks);
	Span Measure(const std::vector<Node2> &src, std::vector<Span> &spans, uint32_t index) const;
	uint32_t Collapse8(const std::vector<Node2> &src, const std::vector<Span> &spans, uint32_t index,
		const uint8_t *marks, bool &any_chancy);

	static int HitChildren8(const Node8 &n, const RayPack8 &r, float tmax, float *tnear);
	static int HitTri8(const Tri8 &p, const Ray &ray, float tmax, bool upto, float *t, float *u, float *v);

	// How far past a hit another still counts as being at the same distance.
	// Two triangles in one plane come out a hundred thousandth apart at
	// worst; this is a hundred times that.
	static float AsGoodAs(float t) { return t + (t * (1.0f / 1024.0f) + 0.01f); }

	// Is the point of a triangle with these weights within a hair, a fiftieth
	// of a unit, of one of its edges? The boxes are a thousandth of that out.
	static bool NearEdge(const Tri8 &p, int lane, float u, float v)
	{
		const float kHair2 = 0.02f * 0.02f;
		const Vec3 e1(p.e1x[lane], p.e1y[lane], p.e1z[lane]), e2(p.e2x[lane], p.e2y[lane], p.e2z[lane]);
		const Vec3 n = Cross(e1, e2), e3 = e2 - e1;
		const float area2 = Dot(n, n);		// twice the area, squared
		const float w = 1.0f - u - v;
		// the distance to an edge is the weight of the corner across from it
		// times that corner's height, which is twice the area over the edge
		return v * v * area2 < kHair2 * Dot(e1, e1) || u * u * area2 < kHair2 * Dot(e2, e2)
			|| w * w * area2 < kHair2 * Dot(e3, e3);
	}
	// which lane the lowest set bit of a mask is in
	static int Lowest(int mask)
	{
#ifdef _MSC_VER
		return (int)_tzcnt_u32((unsigned)mask);
#else
		return __builtin_ctz((unsigned)mask);
#endif
	}

	std::vector<Node8>	nodes8_;	// nodes8_[0] is the root
	std::vector<Tri8>	tris8_;
#endif
};

inline int Bvh::HitChildren(const Node &n, const RayPack &r, float tmax, float *tnear)
{
	const __m128 x0 = _mm_mul_ps(_mm_sub_ps(_mm_loadu_ps(n.lox), r.ox), r.ix);
	const __m128 x1 = _mm_mul_ps(_mm_sub_ps(_mm_loadu_ps(n.hix), r.ox), r.ix);
	const __m128 y0 = _mm_mul_ps(_mm_sub_ps(_mm_loadu_ps(n.loy), r.oy), r.iy);
	const __m128 y1 = _mm_mul_ps(_mm_sub_ps(_mm_loadu_ps(n.hiy), r.oy), r.iy);
	const __m128 z0 = _mm_mul_ps(_mm_sub_ps(_mm_loadu_ps(n.loz), r.oz), r.iz);
	const __m128 z1 = _mm_mul_ps(_mm_sub_ps(_mm_loadu_ps(n.hiz), r.oz), r.iz);
	const __m128 enter = _mm_max_ps(_mm_max_ps(_mm_min_ps(x0, x1), _mm_min_ps(y0, y1)),
		_mm_max_ps(_mm_min_ps(z0, z1), r.tmin));
	const __m128 leave = _mm_min_ps(_mm_min_ps(_mm_max_ps(x0, x1), _mm_max_ps(y0, y1)),
		_mm_min_ps(_mm_max_ps(z0, z1), _mm_set1_ps(tmax)));
	_mm_storeu_ps(tnear, enter);
	return _mm_movemask_ps(_mm_cmple_ps(enter, leave));
}

// Moller-Trumbore
inline bool Bvh::HitTri(const Tri &tri, const Ray &ray, float tmax, float &t, float &u, float &v)
{
	const Vec3 p = Cross(ray.d, tri.e2);
	const float det = Dot(tri.e1, p);
	if (det > -1e-12f && det < 1e-12f)
		return false;
	const float inv = 1.0f / det;
	const Vec3 s = ray.o - tri.p0;
	u = Dot(s, p) * inv;
	if (u < 0.0f || u > 1.0f)
		return false;
	const Vec3 q = Cross(s, tri.e1);
	v = Dot(ray.d, q) * inv;
	if (v < 0.0f || u + v > 1.0f)
		return false;
	t = Dot(tri.e2, q) * inv;
	return t > ray.tmin && t < tmax;
}

template <class F>
bool Bvh::IntersectIf(const Ray &ray, Hit &hit, F accept) const
{
	if (nodes_.empty())
		return false;

	// the lowest set bit of a 4 bit mask
	static const uint8_t kFirst[16] = {0, 0, 1, 0, 2, 0, 1, 0, 3, 0, 1, 0, 2, 0, 1, 0};

	const RayPack pack(ray);
	struct Entry { uint32_t node; float tnear; };
	Entry stack[128];
	int sp = 0;
	uint32_t ni = 0;
	float tmax = ray.tmax;
	bool found = false;

	for (;;)
	{
		const Node &n = nodes_[ni];
		float tnear[4];
		int mask = HitChildren(n, pack, tmax, tnear);

		// nearest child first, so that a hit in it can rule the others out
		Entry inner[4];
		int num_inner = 0;
		while (mask)
		{
			int c = kFirst[mask];
			for (int rest = mask & (mask - 1); rest; rest &= rest - 1)
				if (tnear[kFirst[rest]] < tnear[c])
					c = kFirst[rest];
			mask &= ~(1 << c);
			if (tnear[c] >= tmax)
				break;		// and so are all the others

			if (n.count[c])
			{
				for (uint32_t i = 0; i < n.count[c]; i++)
				{
					const Tri &tri = tris_[n.child[c] + i];
					float t, u, v;
					if (HitTri(tri, ray, tmax, t, u, v) && accept(tri.index, u, v))
					{
						tmax = t;
						hit.t = t;
						hit.u = u;
						hit.v = v;
						hit.tri = tri.index;
						found = true;
					}
				}
			}
			else if (n.child[c] != kNone)
				inner[num_inner++] = {n.child[c], tnear[c]};
		}

		if (num_inner && inner[0].tnear < tmax)
		{
			// into the nearest; the rest wait, the farthest at the bottom
			for (int k = num_inner - 1; k > 0; k--)
				if (sp < 128)
					stack[sp++] = inner[k];
			ni = inner[0].node;
			continue;
		}
		for (;;)
		{
			if (!sp)
				return found;
			const Entry e = stack[--sp];
			if (e.tnear < tmax)
			{
				ni = e.node;
				break;
			}
		}
	}
}

template <class F>
bool Bvh::AnyHit(const Ray &ray, F blocks) const
{
	if (nodes_.empty())
		return false;

	const RayPack pack(ray);
	uint32_t stack[128];
	int sp = 0;
	uint32_t ni = 0;

	for (;;)
	{
		const Node &n = nodes_[ni];
		float tnear[4];
		const int mask = HitChildren(n, pack, ray.tmax, tnear);

		bool descend = false;
		for (int c = 0; c < 4; c++)
		{
			if (!(mask & (1 << c)))
				continue;
			if (n.count[c])
			{
				for (uint32_t i = 0; i < n.count[c]; i++)
				{
					const Tri &tri = tris_[n.child[c] + i];
					float t, u, v;
					if (HitTri(tri, ray, ray.tmax, t, u, v) && blocks(tri.index, u, v))
						return true;
				}
			}
			else if (n.child[c] == kNone)
				continue;
			else if (descend)
			{
				if (sp < 128)
					stack[sp++] = n.child[c];
			}
			else
			{
				ni = n.child[c];
				descend = true;
			}
		}
		if (descend)
			continue;
		if (!sp)
			return false;
		ni = stack[--sp];
	}
}

#ifdef PT_AVX2_KERNELS

// as HitChildren, for eight children
inline int Bvh::HitChildren8(const Node8 &n, const RayPack8 &r, float tmax, float *tnear)
{
	const __m256 x0 = _mm256_mul_ps(_mm256_sub_ps(_mm256_loadu_ps(n.lox), r.ox), r.ix);
	const __m256 x1 = _mm256_mul_ps(_mm256_sub_ps(_mm256_loadu_ps(n.hix), r.ox), r.ix);
	const __m256 y0 = _mm256_mul_ps(_mm256_sub_ps(_mm256_loadu_ps(n.loy), r.oy), r.iy);
	const __m256 y1 = _mm256_mul_ps(_mm256_sub_ps(_mm256_loadu_ps(n.hiy), r.oy), r.iy);
	const __m256 z0 = _mm256_mul_ps(_mm256_sub_ps(_mm256_loadu_ps(n.loz), r.oz), r.iz);
	const __m256 z1 = _mm256_mul_ps(_mm256_sub_ps(_mm256_loadu_ps(n.hiz), r.oz), r.iz);
	const __m256 enter = _mm256_max_ps(_mm256_max_ps(_mm256_min_ps(x0, x1), _mm256_min_ps(y0, y1)),
		_mm256_max_ps(_mm256_min_ps(z0, z1), r.tmin));
	const __m256 leave = _mm256_min_ps(_mm256_min_ps(_mm256_max_ps(x0, x1), _mm256_max_ps(y0, y1)),
		_mm256_min_ps(_mm256_max_ps(z0, z1), _mm256_set1_ps(tmax)));
	_mm256_storeu_ps(tnear, enter);
	return _mm256_movemask_ps(_mm256_cmp_ps(enter, leave, _CMP_LE_OQ));
}

// HitTri for the eight triangles of a leaf at once: a bit per lane that is
// hit, and where. upto: a hit at tmax itself counts as well.
//
// Every lane has to come to the very bits HitTri comes to for that triangle.
// Built for AVX2 the compiler fuses HitTri's multiplications and additions
// into single operations, each rounding once where two would round twice,
// and which it fuses decides the last bit of the result. What follows is
// that arithmetic spelled out, as MSVC 14.38 makes it: in a cross product
// the first product of each component is the fused one, and in a dot product
// the y and z terms are fused onto the x term, except in u's where the x and
// z terms are fused onto the y term. Change either and the two trees stop
// agreeing; nothing breaks, but a picture no longer comes out the same from
// both.
inline int Bvh::HitTri8(const Tri8 &p, const Ray &ray, float tmax, bool upto, float *t, float *u, float *v)
{
	const __m256 zero = _mm256_setzero_ps(), one = _mm256_set1_ps(1.0f);
	const __m256 dx = _mm256_set1_ps(ray.d.x), dy = _mm256_set1_ps(ray.d.y), dz = _mm256_set1_ps(ray.d.z);
	const __m256 e1x = _mm256_loadu_ps(p.e1x), e1y = _mm256_loadu_ps(p.e1y), e1z = _mm256_loadu_ps(p.e1z);
	const __m256 e2x = _mm256_loadu_ps(p.e2x), e2y = _mm256_loadu_ps(p.e2y), e2z = _mm256_loadu_ps(p.e2z);

	// p = Cross(ray.d, e2), det = Dot(e1, p)
	const __m256 px = _mm256_fmsub_ps(dy, e2z, _mm256_mul_ps(dz, e2y));
	const __m256 py = _mm256_fmsub_ps(dz, e2x, _mm256_mul_ps(dx, e2z));
	const __m256 pz = _mm256_fmsub_ps(dx, e2y, _mm256_mul_ps(dy, e2x));
	const __m256 det = _mm256_fmadd_ps(e1z, pz, _mm256_fmadd_ps(e1y, py, _mm256_mul_ps(e1x, px)));
	__m256 ok = _mm256_or_ps(_mm256_cmp_ps(det, _mm256_set1_ps(-1e-12f), _CMP_LE_OQ),
		_mm256_cmp_ps(det, _mm256_set1_ps(1e-12f), _CMP_GE_OQ));
	const __m256 inv = _mm256_div_ps(one, det);

	// s = ray.o - p0, u = Dot(s, p) * inv
	const __m256 sx = _mm256_sub_ps(_mm256_set1_ps(ray.o.x), _mm256_loadu_ps(p.p0x));
	const __m256 sy = _mm256_sub_ps(_mm256_set1_ps(ray.o.y), _mm256_loadu_ps(p.p0y));
	const __m256 sz = _mm256_sub_ps(_mm256_set1_ps(ray.o.z), _mm256_loadu_ps(p.p0z));
	const __m256 uu = _mm256_mul_ps(_mm256_fmadd_ps(pz, sz, _mm256_fmadd_ps(px, sx, _mm256_mul_ps(py, sy))), inv);
	ok = _mm256_and_ps(ok, _mm256_and_ps(_mm256_cmp_ps(uu, zero, _CMP_GE_OQ), _mm256_cmp_ps(uu, one, _CMP_LE_OQ)));

	// q = Cross(s, e1), v = Dot(ray.d, q) * inv
	const __m256 qx = _mm256_fmsub_ps(sy, e1z, _mm256_mul_ps(sz, e1y));
	const __m256 qy = _mm256_fmsub_ps(sz, e1x, _mm256_mul_ps(sx, e1z));
	const __m256 qz = _mm256_fmsub_ps(sx, e1y, _mm256_mul_ps(sy, e1x));
	const __m256 vv = _mm256_mul_ps(_mm256_fmadd_ps(dz, qz, _mm256_fmadd_ps(dy, qy, _mm256_mul_ps(dx, qx))), inv);
	ok = _mm256_and_ps(ok, _mm256_and_ps(_mm256_cmp_ps(vv, zero, _CMP_GE_OQ),
		_mm256_cmp_ps(_mm256_add_ps(uu, vv), one, _CMP_LE_OQ)));

	// t = Dot(e2, q) * inv
	const __m256 tt = _mm256_mul_ps(_mm256_fmadd_ps(e2z, qz, _mm256_fmadd_ps(e2y, qy, _mm256_mul_ps(e2x, qx))), inv);
	const __m256 far_end = _mm256_set1_ps(tmax);
	ok = _mm256_and_ps(ok, _mm256_and_ps(_mm256_cmp_ps(tt, _mm256_set1_ps(ray.tmin), _CMP_GT_OQ),
		upto ? _mm256_cmp_ps(tt, far_end, _CMP_LE_OQ) : _mm256_cmp_ps(tt, far_end, _CMP_LT_OQ)));

	_mm256_storeu_ps(t, tt);
	_mm256_storeu_ps(u, uu);
	_mm256_storeu_ps(v, vv);
	return _mm256_movemask_ps(ok);
}

template <class F>
bool Bvh::Intersect8(const Ray &ray, Hit &hit, F accept, bool &unsure) const
{
	unsure = false;
	if (nodes8_.empty())
		return false;

	const RayPack8 pack(ray);
	struct Entry { uint32_t node; float tnear; };
	Entry stack[kStack8];
	int sp = 0;
	uint32_t ni = 0;
	float tmax = ray.tmax;		// the nearest hit so far
	float reach = ray.tmax;		// how far to look: a little past it, for what is as good as it
	float second = FLT_MAX;		// the next nearest
	const Tri8 *best = nullptr;
	int best_lane = 0;

	for (;;)
	{
		const Node8 &n = nodes8_[ni];
		float tnear[8];
		int mask = HitChildren8(n, pack, reach, tnear);

		// the children the ray enters, nearest first, so that a hit in one
		// can rule the others out
		int order[8], num = 0;
		for (; mask; mask &= mask - 1)
		{
			const int c = Lowest(mask);
			int k = num++;
			for (; k > 0 && tnear[order[k - 1]] > tnear[c]; k--)
				order[k] = order[k - 1];
			order[k] = c;
		}

		Entry inner[8];
		int num_inner = 0;
		for (int j = 0; j < num; j++)
		{
			const int c = order[j];
			if (tnear[c] > reach)
				break;		// and so are all the others

			if (n.count[c])
			{
				const Tri8 &p = tris8_[n.child[c]];
				float t[8], u[8], v[8];
				for (int m = HitTri8(p, ray, reach, true, t, u, v); m; m &= m - 1)
				{
					const int k = Lowest(m);
					if (t[k] > reach || (((p.chancy >> k) & 1) && !accept(p.index[k], u[k], v[k])))
						continue;
					if (t[k] < tmax)
					{
						if (best)
							second = tmax;		// what was nearest is no more than next nearest
						tmax = t[k];
						reach = AsGoodAs(tmax);
						hit.t = t[k];
						hit.u = u[k];
						hit.v = v[k];
						hit.tri = p.index[k];
						best = &p;
						best_lane = k;
					}
					else if (best && t[k] < second)
						second = t[k];
				}
			}
			else
				inner[num_inner++] = {n.child[c], tnear[c]};
		}

		if (num_inner && inner[0].tnear <= reach)
		{
			// into the nearest; the rest wait, the farthest at the bottom
			for (int k = num_inner - 1; k > 0; k--)
				if (sp < kStack8)
					stack[sp++] = inner[k];
			ni = inner[0].node;
			continue;
		}
		for (;;)
		{
			if (!sp)
			{
				if (!best)
					return false;
				unsure = second <= reach || NearEdge(*best, best_lane, hit.u, hit.v);
				return true;
			}
			const Entry e = stack[--sp];
			if (e.tnear <= reach)
			{
				ni = e.node;
				break;
			}
		}
	}
}

template <class F>
int Bvh::AnyHit8(const Ray &ray, F kind) const
{
	if (nodes8_.empty())
		return 0;

	const uint32_t kMarked = 0x80000000u;	// on a waiting node: a chancy triangle may be under it
	const RayPack8 pack(ray);
	uint32_t stack[kStack8];
	int sp = 0;
	uint32_t ni = 0;
	bool stopped = false;		// by something AnyHit is sure to find as well
	bool doubt = false;			// something stops it that AnyHit might take to be missed

	for (;;)
	{
		const Node8 &n = nodes8_[ni];
		float tnear[8];
		uint32_t next = kNone;

		for (int mask = HitChildren8(n, pack, ray.tmax, tnear); mask; mask &= mask - 1)
		{
			const int c = Lowest(mask);
			const bool marked = ((n.chancy >> c) & 1) != 0;
			// once the ray is stopped only a chancy triangle can still change
			// the answer, to "unsure"
			if (stopped && !marked)
				continue;

			if (n.count[c])
			{
				const Tri8 &p = tris8_[n.child[c]];
				float t[8], u[8], v[8];
				for (int m = HitTri8(p, ray, ray.tmax, false, t, u, v); m; m &= m - 1)
				{
					const int k = Lowest(m);
					if (stopped && !((p.chancy >> k) & 1))
						continue;
					const int what = ((p.ask >> k) & 1) ? kind(p.index[k], u[k], v[k]) : 1;
					if (what == 2)
						return 2;
					if (what == 1)
					{
						if (NearEdge(p, k, u[k], v[k]))
							doubt = true;
						else
							stopped = true;
					}
				}
			}
			else if (next == kNone)
				next = n.child[c] | (marked ? kMarked : 0);
			else if (sp < kStack8)
				stack[sp++] = n.child[c] | (marked ? kMarked : 0);
		}

		for (;;)
		{
			if (next == kNone)
			{
				if (!sp)
					return stopped ? 1 : (doubt ? 2 : 0);
				next = stack[--sp];
			}
			if (!stopped || (next & kMarked))
				break;
			next = kNone;
		}
		ni = next & ~kMarked;
	}
}

#endif	// PT_AVX2_KERNELS

} // namespace PT_NS
