// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Jonathan Ferguson
//
// The CPU backend can be built twice into one program: once for processors
// with AVX2 and once for those without, the right one being picked when the
// backend is made (pt_cpu_pick.cpp). Each build keeps everything it defines
// in a namespace of its own, named by PT_NS, so that the two cannot be taken
// for one another when they are linked together. Built once, as it also is
// where the RTX backend borrows the world builder, the namespace is pt.
#pragma once

#ifndef PT_NS
#define PT_NS pt
#endif
