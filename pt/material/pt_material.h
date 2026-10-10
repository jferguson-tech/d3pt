/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Jonathan Ferguson */
/*
Makes, from nothing but the colours of a texture, what a path tracer wants
to know about the surface and old game art never recorded: which way its
small details face, how rough it is from place to place, and which of it is
metal.

The art it is meant for was painted by hand in a few hundred colours, with
its own light in it: raised edges were given a highlight on the side towards
the top left of the picture and a shadow on the other. That painted light is
the best evidence of shape there is, so it is read back as shape. Where
nothing was painted, dark is taken to be deep. Once the shape is known the
painted light is no longer wanted in the colours, where it would light every
raised edge a second time and from the wrong side as often as not, so the
picture can be had back with it taken out.

Metal is told by colour. Steel was painted grey and what covers or is not
metal vivid: rust, paint, wood, cloth, skin. And it was painted as dark as it
looks, not as bright as it reflects, so what a metal of a given painted
colour reflects is worked out here too.

Light is told by brightness. A lamp or a lit screen was painted far brighter
than what is round it, so in a picture known to show one, the part that
gives off the light can be told from its frame.

It knows nothing about any game or renderer, and reads and writes no files.
*/
#ifndef PT_MATERIAL_H
#define PT_MATERIAL_H

#include <math.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
Raised whenever the map made from a given picture changes, so that whoever
keeps maps from one run to the next can tell which were made by an older
reading.
*/
#define PT_MATERIAL_VERSION		4

typedef struct pt_material_from_s
{
	int		repeats;		/* the picture tiles both ways, as a wall's does, so
							   its edges are worked across; 0 for one that
							   stands alone, as a model's skin does */
	int		painted_light;	/* highlights and shadows are read as shape lit from
							   the top left. 0 where the picture's up is not the
							   surface's, as on a skin wrapped round a model:
							   then only "dark is deep" is used */
	float	bump;			/* 1 = relief as read; more is deeper, 0 is flat */
	float	roughness;		/* the material's own, 0 mirror - 1 matte; the
							   picture varies it about this */
	float	metallic;		/* how metallic its metal is: 1, or 0 for what is
							   known not to be metal. The picture says where
							   the metal is: what was painted grey or nearly
							   so is, what was painted vivid is rust, paint,
							   wood, cloth or skin, and what is black is a gap */
	int		metal_known;	/* it is known to be metal, so only what covers
							   the metal is looked for. 0 where nothing is
							   known: then grey that could as well be stone
							   or cloth is held to more strictly */
	float	metal_edge;		/* texels of the picture that the edge of a patch
							   of metal is dithered over: within that much of
							   the edge, texels are metal the more often the
							   further into the patch they lie. 0 = the edge
							   is a line */
	float	delight;		/* how much of the painted light is taken out of
							   the colours: 0 none, 1 all that was read. Only
							   with painted_light */
} pt_material_from_t;

typedef struct pt_material_maps_s
{
	uint32_t	*detail;	/* pt_material_detail_scale() times as wide and as
							   high as the picture. In each pixel R and G are
							   the x and y of a unit normal, x to the right and
							   y down the picture, each stored as (n + 1) / 2;
							   its z, out of the picture, is what is left of
							   its length. B is how metallic the surface is
							   there: metallic or nothing, never between.
							   A is how rough */
	int			detail_width, detail_height;
	uint32_t	*colour;	/* the picture with its painted light taken out, or
							   NULL where there was none to take: then the
							   picture itself is as good */
	int			colour_width, colour_height;
} pt_material_maps_t;

/*
How many times finer than the picture its detail map is, each way. A picture
that repeats is stretched over whole walls and seen from close by, so its
map is made finer than it is; one that stands alone gets a map of its size.
*/
int pt_material_detail_scale(int width, int height, int repeats);

/*
pixels: width * height of R,G,B,A bytes in memory, top row first, colours
as they are shown (not linear light).

Fills in maps and returns 1; each map is made with malloc for the caller to
free. Returns 0, with both maps NULL, if the picture is too large (over 2048
either way) or there was no memory for it.
*/
int pt_material_read(const uint32_t *pixels, int width, int height, const pt_material_from_t *from, pt_material_maps_t *maps);

/*
The same picture's height as it was read, for looking at: one byte a pixel
at the detail map's size, 128 level, 64 a texel lower. Made with malloc, or
NULL.
*/
unsigned char *pt_material_height(const uint32_t *pixels, int width, int height, const pt_material_from_t *from);

/*
What is lit in a picture known to show a lamp, a lit screen or a lit sign:
the picture again, the same size, black wherever it gives off no light. The
art was painted with its lamps far brighter than what is round them, so a
texel is lit if it is far brighter than the picture is for the most part and
belongs to a patch that is brighter still somewhere: the lamp and the glow
painted round it, not its frame; the letters, not the sign. A red lamp
counts for as bright as a white one. What lies within a few texels of a lit
patch and is as bright and of the same colour is lit with it, as the bright
rings of one lens are; so is what is strongly coloured and lies between lit
texels of its own colour, as its dark rings do; and gaps a texel wide in
what is lit are closed. repeats is as in pt_material_from_t.

NULL if nothing in the picture stands apart from the rest of it: it is
unlit, or of one thing all over, as a crystal or a sheet of lava is, and
then all of it gives off light or none of it does. NULL too if it is too
large or there is no memory. Made with malloc.

share is given how much of all the light the picture's colours stand for
is in the part that is lit, 0 - 1. Where that is small, what was found is a
few bright specks in a picture that is not dark, and a light that the whole
picture stood for until now would be crowded into them.

This is for pictures that are known to be of something lit. Colour and
brightness alone do not tell a lamp from a bright yellow stripe, so what a
picture is of has to come from elsewhere: its name, or the map it is in.
*/
uint32_t *pt_material_glow(const uint32_t *pixels, int width, int height, int repeats, float *share);

/*
What a metal reflects, worked out from the colour it was painted. Art made
to be looked at gives steel the dark grey it shows in a dim room, a small
part of what steel reflects, and a metal that reflected no more than that
would be black. rgb and out are linear light, 0 - 1, and may be the same.

level is what a metal painted PT_MATERIAL_METAL_PAINTED bright is taken to
reflect. One painted brighter reflects more, by the square root, which
halves the painting's contrast, and none reflects less than it was painted.
The hue is kept, but the more a colour is brightened the less is made of
it, or a dark brown would come out orange; of what is next to black no hue
is known. 0 or less leaves the colour as it is.

How bright is right depends on the rest of the picture. Steel reflects half
the light and more, but a level of 0.5 is for art whose other colours are
what those things reflect. Old game art is far darker than that all over,
and lit to match: beside it such a metal is a sheet of white.

A path tracer does this where it shades, not to the picture: what a surface
emits and what it scatters where it is not metal go by the colour as painted.
*/
#define PT_MATERIAL_METAL_PAINTED	0.012f
#define PT_MATERIAL_METAL_HUE_KEPT	0.6f

static inline void pt_material_metal_colour(const float *rgb, float level, float *out)
{
	const float r = rgb[0], g = rgb[1], b = rgb[2];
	const float most = r > g ? (r > b ? r : b) : (g > b ? g : b);
	float	y, bright, keep, over;

	if (level <= 0.0f)
	{
		out[0] = r;
		out[1] = g;
		out[2] = b;
		return;
	}
	y = 0.2126f * r + 0.7152f * g + 0.0722f * b;
	if (y < 1.0e-6f)
		y = 1.0e-6f;
	bright = level * sqrtf(y * (1.0f / PT_MATERIAL_METAL_PAINTED));
	if (bright < y)
		bright = y;
	keep = most * (1.0f / 0.004f);
	keep = (PT_MATERIAL_METAL_HUE_KEPT + (1.0f - PT_MATERIAL_METAL_HUE_KEPT) * y / bright) * (keep < 1.0f ? keep : 1.0f);
	out[0] = (1.0f + (r / y - 1.0f) * keep) * bright;
	out[1] = (1.0f + (g / y - 1.0f) * keep) * bright;
	out[2] = (1.0f + (b / y - 1.0f) * keep) * bright;
	over = out[0] > out[1] ? (out[0] > out[2] ? out[0] : out[2]) : (out[1] > out[2] ? out[1] : out[2]);
	if (over > 1.0f)
	{
		out[0] /= over;
		out[1] /= over;
		out[2] /= over;
	}
}

#ifdef __cplusplus
}
#endif

#endif
