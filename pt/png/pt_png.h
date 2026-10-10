/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Jonathan Ferguson */
/*
Writes a picture as a PNG file: 8 bits each of red, green and blue, packed
with its own deflate, so nothing else is needed to build or run it.
*/
#ifndef PT_PNG_H
#define PT_PNG_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
pixels: width * height of them, bytes R,G,B,A in memory (A is ignored), top
row first. Returns 1 if the file was written, 0 if it could not be.
*/
int pt_png_write(const char *path, const uint32_t *pixels, int width, int height);

#ifdef __cplusplus
}
#endif

#endif
