/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Jonathan Ferguson */
/*
A shallow water simulation for one flat body of liquid, and the pictures
a renderer needs from it: where the surface stands and which way it tilts,
how sharply it curves, which is what gathers light on whatever lies below
(caustics), how far up its banks it has wetted, and where the waves have
beaten the surface to froth.

Each cell of a grid holds how high the surface stands and how fast the
liquid flows across to its neighbours. Waves travel as fast as the depth
under them lets them, so they slow, bunch up and turn over a shallow bed,
pass round whatever stands in the liquid, and slosh between its banks.

It knows nothing about any renderer. The host says where there is liquid
and how deep, steps it, pokes it where things touch the surface, and hands
the pictures on as textures.
*/
#ifndef PT_WATER_H
#define PT_WATER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct pt_water_s pt_water_t;

/* slopes are stored as slope * PT_WATER_SLOPE_SCALE + 0.5, clamped to 0-1 */
#define PT_WATER_SLOPE_SCALE	1.0f
/* heights are stored as height / (2 * PT_WATER_HEIGHT_MAX) + 0.5, clamped to
   0-1, in sixteen bits: B the upper eight, A the lower */
#define PT_WATER_HEIGHT_MAX		8.0f
/* How sharply the surface curves, the change of slope per unit, is stored
   as a byte b with (b - 128) / 127 = s, the curvature being
   s * |s| * PT_WATER_CURVE_MAX: finest where it curves least */
#define PT_WATER_CURVE_MAX		0.125f
#define PT_WATER_RIPPLE_MAX		4.0f
/* The byte that says whether there is liquid: 0 where there is none, and
   from PT_WATER_WET to 255 where there is, for the liquid having lately
   stood from level to PT_WATER_HEIGHT_MAX above it */
#define PT_WATER_WET			160

/* covers the rectangle in cells of about cell_size; at most max_cells a side */
pt_water_t *pt_water_create(float min_x, float min_y, float max_x, float max_y, float cell_size, int max_cells);
void pt_water_destroy(pt_water_t *w);

int pt_water_width(const pt_water_t *w);
int pt_water_height(const pt_water_t *w);
float pt_water_cell(const pt_water_t *w);	/* the size the cells came out */

/*
Says where in the rectangle there is liquid. Until the first call all of it
is; after that only what the triangles given so far cover, and waves bounce
off the rest as off a bank.
*/
void pt_water_cover(pt_water_t *w, const float a[2], const float b[2], const float c[2]);

/*
Says how deep the liquid is: a triangle of whatever lies under the surface,
x, y and z, with z counted from the level of the liquid, so negative below
it. The shallowest thing under each cell is its bed. Until the first call
the liquid is taken to be 96 units deep everywhere; after that, wherever no
triangle was given.
*/
void pt_water_bed(pt_water_t *w, const float a[3], const float b[3], const float c[3]);

/*
The liquid over this triangle is in a stream running at vx, vy units per
second, which carries the shape of the surface along with it.
*/
void pt_water_current(pt_water_t *w, const float a[2], const float b[2], const float c[2], float vx, float vy);

/* push the surface down around a point: amount is in units of height */
void pt_water_disturb(pt_water_t *w, float x, float y, float radius, float amount);

/*
Something of this radius at the surface has been moving at vx, vy for dt
seconds: the surface rises ahead of it and sinks behind, and the liquid it
passes through is dragged along. amount is how much of it is in the liquid,
in units of height.
*/
void pt_water_move(pt_water_t *w, float x, float y, float radius, float vx, float vy, float amount, float dt);

/*
Froth. amount is how readily it forms, 1 being usual and 0, as it is to
begin with, not at all; life is how many seconds it takes half of it to
go. It forms by itself where waves stand steep or break and where a stream
runs into something, drifts with the liquid, grows old and goes, and
shows in the foam picture. shore is how much of it there always is where
the liquid laps against its banks, 0 for none and 1 for a good deal; that
comes and goes with the waves.
*/
void pt_water_foaming(pt_water_t *w, float amount, float life, float shore);

/* something has beaten the surface to froth around a point: amount is how
   much of the middle of it is covered, 1 being all */
void pt_water_churn(pt_water_t *w, float x, float y, float radius, float amount);

/*
Advance by dt seconds. Waves travel at the square root of gravity times the
depth, in units per second; damping is the share of the liquid's motion
lost per second (0-1). The liquid moves in steps of dt, or smaller ones
where it is too deep for that, so how often this is called is how smoothly
it moves.
*/
void pt_water_step(pt_water_t *w, float dt, float gravity, float damping);

/*
Is the liquid at rest: level, with nothing flowing, no froth going, and
nothing done to it since it was last stepped? Stepping it then changes
nothing, and its pictures are what they were. A new one is not until it
has been stepped once.
*/
int pt_water_still(const pt_water_t *w);

/*
Where a wave has broken hard enough to throw up spray since this was last
asked: at most PT_WATER_SPRAY_MAX places, each x, y and how hard,
upwards of 0.2. Returns how many; they stay valid until the next call that
changes the simulation.
*/
#define PT_WATER_SPRAY_MAX	32
int pt_water_spray(pt_water_t *w, const float **at);

/*
The pictures, width * height pixels of R,G,B,A bytes, row 0 at min_y.
waves: R and G are the slopes in x and y, B and A the height.
caustics: R and G are how sharply the surface curves along x and along y,
as in the wave picture made last and times `strength`. Light going
straight down through the surface and d further is brighter by
1 / |(1 + k d R)(1 + k d G)|, k being 1 - 1 / the liquid's refractive index.
B is how ruffled the surface is with ripples too fine for the cells to
hold, which gather light in the same way and are for whoever draws it to
make up: 0 none, 1 as much as there can be, times `strength` and divided
by PT_WATER_RIPPLE_MAX. There are always a few, and more where waves are
or a stream runs.
A says whether there is liquid, there or in a cell next to it, and how
high it has lately stood, which is how far up its banks are wet.
foam: R is how much of the cell froth covers. G is how old it is, 0 just
made to 1. B and A are how fast the liquid under it is moving in x and y:
units per second + 128, from 1 to 255.
Both stay valid until the next call that changes the simulation.
*/
const uint32_t *pt_water_waves(pt_water_t *w, float wave_scale);
/*
How far above level the surface stands at a point, as the wave picture made
with this wave_scale has it. Returns 0 where there is no liquid; covered,
if given, says whether there is.
*/
float pt_water_height_at(const pt_water_t *w, float x, float y, float wave_scale, int *covered);
/* how far the surface stood from level, at most, in the picture last made */
float pt_water_reach(const pt_water_t *w);
const uint32_t *pt_water_caustics(pt_water_t *w, float strength);
const uint32_t *pt_water_foam(pt_water_t *w);

#ifdef __cplusplus
}
#endif

#endif
