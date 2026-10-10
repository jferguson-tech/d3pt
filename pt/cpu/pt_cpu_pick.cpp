// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Jonathan Ferguson
//
// The CPU backend is in the program twice: built for processors with AVX2,
// and built for any. This picks between the two when the backend is made:
// the one for AVX2 wherever it can run, unless the host asks for the other.

#include "../include/pt.h"

#if defined(_MSC_VER)
#include <intrin.h>
#include <immintrin.h>
#else
#include <cpuid.h>
#endif

extern "C" pt_backend_t *pt_cpu_create_sse(const pt_create_t *ci, char *err, int errlen);
extern "C" pt_backend_t *pt_cpu_create_avx2(const pt_create_t *ci, char *err, int errlen);

namespace {

void Cpuid(unsigned leaf, unsigned sub, unsigned regs[4])
{
#if defined(_MSC_VER)
	int r[4];
	__cpuidex(r, (int)leaf, (int)sub);
	for (int i = 0; i < 4; i++)
		regs[i] = (unsigned)r[i];
#else
	__cpuid_count(leaf, sub, regs[0], regs[1], regs[2], regs[3]);
#endif
}

// which sets of registers the system keeps across a switch of threads
unsigned long long SavedState()
{
#if defined(_MSC_VER)
	return _xgetbv(0);
#else
	unsigned lo, hi;
	__asm__("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0));
	return ((unsigned long long)hi << 32) | lo;
#endif
}

// Everything a compiler told to build for AVX2 may use: AVX2 itself, fused
// multiply-add and the bit manipulation instructions that came with them,
// on a system that keeps the wide registers.
bool CanRunAvx2()
{
	unsigned r[4];

	Cpuid(0, 0, r);
	if (r[0] < 7)
		return false;

	Cpuid(1, 0, r);
	const unsigned fma = 1u << 12, movbe = 1u << 22, popcnt = 1u << 23, osxsave = 1u << 27, avx = 1u << 28;
	const unsigned need1 = fma | movbe | popcnt | osxsave | avx;
	if ((r[2] & need1) != need1)
		return false;
	if ((SavedState() & 6) != 6)
		return false;

	Cpuid(7, 0, r);
	const unsigned bmi1 = 1u << 3, avx2 = 1u << 5, bmi2 = 1u << 8;
	const unsigned need7 = bmi1 | avx2 | bmi2;
	if ((r[1] & need7) != need7)
		return false;

	Cpuid(0x80000000u, 0, r);
	if (r[0] < 0x80000001u)
		return false;
	Cpuid(0x80000001u, 0, r);
	return (r[2] & (1u << 5)) != 0;		// lzcnt
}

} // namespace

extern "C" pt_backend_t *pt_cpu_create(const pt_create_t *ci, char *err, int errlen)
{
	if (ci->simd != 1 && CanRunAvx2())
		return pt_cpu_create_avx2(ci, err, errlen);
	return pt_cpu_create_sse(ci, err, errlen);
}
