/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Jonathan Ferguson */

#include "pt_water.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* Deeper than this the bed makes waves no faster: it bounds how fast they
   travel, and so how many steps a frame takes. */
#define DEPTH_MAX		256.0f
#define DEPTH_MIN		4.0f
#define DEPTH_UNKNOWN	96.0f	/* where nothing was said about the bed */

struct pt_water_s
{
	int			width, height;
	float		min_x, min_y;
	float		cell;			/* world units per cell */
	float		*h;				/* how far the surface stands above level */
	/* How fast the liquid flows across the faces between cells: u across
	   the face on the low x side of each cell and one more at the end of
	   each row, (width + 1) * height of them; v the same in y. */
	float		*u, *v;
	float		*fx, *fy;		/* what crosses each face in a step, laid out as u and v */
	float		*depth;			/* of the liquid at rest, down to the bed */
	float		*current;		/* x, y per cell: the stream the liquid is in; NULL until one is given */
	float		*tmp;
	uint32_t	*waves, *caustics;
	unsigned char *open;	/* per cell: is there liquid here */
	int			covered;		/* has pt_water_cover been called */
	int			sounded;		/* has pt_water_bed been called */
	float		deepest;		/* the largest depth, for the size of a step */
	float		leftover;		/* time not yet simulated */
	float		reach;			/* furthest from level in the last wave picture */
	float		tall;			/* the same before wave_scale, or a great deal if no picture was made yet */
	float		shown;			/* the wave_scale of the last wave picture */
	float		*mark;			/* how high the liquid has lately stood in each cell: what it left wet */
	float		*stir;			/* how ruffled each cell is, 0 to 1: see STIR_REST */
	float		*foam;			/* how much of each cell froth covers, 0 to FOAM_MAX; NULL until asked for */
	float		*old;			/* that times how old it is, 0 just made to 1; what is added to foam is new */
	float		*shore;			/* how much the liquid froths of itself at each cell, against its banks; NULL until worked out */
	uint32_t	*froth;			/* the foam picture */
	float		foaming, foam_life, shore_amount;
	int			foamy;			/* is there any froth anywhere */
	float		spray[PT_WATER_SPRAY_MAX * 3];
	int			sprays;
	unsigned	seed;
	int			touched;		/* something has been done to it since the last step */
	int			quiet;			/* the last step left it level and at rest */
	int			settling;		/* a wet mark is still drying or ripples are still dying down */
};

/* how fast the wet a wave left behind it dries, in units of height a second */
#define WET_DRIES		0.3f

/* Ripples too fine for the cells. No liquid lies quite still, so there are
   always a few; a slope, a height above or below level and a stream each
   ruffle it more, up to 1, and it settles by so much a second. */
#define STIR_REST		0.25f
#define STIR_SLOPE		2.0f
#define STIR_HEIGHT		0.5f
#define STIR_STREAM		0.01f
#define STIR_SETTLES	0.4f

/* a surface nowhere further from level than this, in units, with nothing
   flowing faster than FLOW_REST units a second, is at rest */
#define HEIGHT_REST		0.02f
#define FLOW_REST		0.1f

/* froth may pile up past what covers a cell, and then lasts the longer */
#define FOAM_MAX		1.5f
/* a face of liquid steeper than this, as a slope, turns white */
#define FOAM_STEEP		0.18f

/* froth made in one cell; where a wave broke to make it, enough of it at
   once throws up spray */
static void froth(pt_water_t *w, size_t i, float amount, int broke)
{
	amount *= w->foaming;
	if (amount <= 0.0f)
		return;
	w->foam[i] += amount;
	if (w->foam[i] > FOAM_MAX)
		w->foam[i] = FOAM_MAX;
	w->foamy = 1;
	if (broke && amount > 0.2f)
	{
		/* the list is short: when it is full, any of them may give way */
		int slot = w->sprays;
		if (slot == PT_WATER_SPRAY_MAX)
		{
			w->seed = w->seed * 1664525u + 1013904223u;
			slot = (int)((w->seed >> 16) % PT_WATER_SPRAY_MAX);
			if (w->spray[slot * 3 + 2] >= amount)
				return;
		}
		else
			w->sprays++;
		w->spray[slot * 3] = w->min_x + ((float)(i % (size_t)w->width) + 0.5f) * w->cell;
		w->spray[slot * 3 + 1] = w->min_y + ((float)(i / (size_t)w->width) + 0.5f) * w->cell;
		w->spray[slot * 3 + 2] = amount;
	}
}

pt_water_t *pt_water_create(float min_x, float min_y, float max_x, float max_y, float cell_size, int max_cells)
{
	pt_water_t	*w;
	float		size_x = max_x - min_x, size_y = max_y - min_y, larger;
	size_t		count;

	if (size_x <= 0 || size_y <= 0 || cell_size <= 0 || max_cells < 4)
		return NULL;

	/* square cells, coarsened until the longer side fits */
	larger = size_x > size_y ? size_x : size_y;
	if (larger / cell_size > max_cells)
		cell_size = larger / max_cells;

	w = (pt_water_t *)calloc(1, sizeof(*w));
	if (!w)
		return NULL;
	w->min_x = min_x;
	w->min_y = min_y;
	w->cell = cell_size;
	w->width = (int)ceilf(size_x / cell_size);
	w->height = (int)ceilf(size_y / cell_size);
	if (w->width < 2) w->width = 2;
	if (w->height < 2) w->height = 2;

	count = (size_t)w->width * w->height;
	w->h = (float *)calloc(count, sizeof(float));
	w->u = (float *)calloc((size_t)(w->width + 1) * w->height, sizeof(float));
	w->v = (float *)calloc((size_t)w->width * (w->height + 1), sizeof(float));
	w->fx = (float *)calloc((size_t)(w->width + 1) * w->height, sizeof(float));
	w->fy = (float *)calloc((size_t)w->width * (w->height + 1), sizeof(float));
	w->depth = (float *)malloc(count * sizeof(float));
	w->tmp = (float *)calloc(count, sizeof(float));
	w->mark = (float *)calloc(count, sizeof(float));
	w->stir = (float *)calloc(count, sizeof(float));
	w->shown = 1.0f;
	w->waves = (uint32_t *)calloc(count, sizeof(uint32_t));
	w->caustics = (uint32_t *)calloc(count, sizeof(uint32_t));
	w->open = (unsigned char *)malloc(count);
	if (w->open)
		memset(w->open, 1, count);
	if (!w->open || !w->h || !w->u || !w->v || !w->fx || !w->fy || !w->depth || !w->tmp || !w->mark || !w->stir || !w->waves || !w->caustics)
	{
		pt_water_destroy(w);
		return NULL;
	}
	for (count = 0; count < (size_t)w->width * w->height; count++)
		w->depth[count] = DEPTH_UNKNOWN;
	w->deepest = DEPTH_UNKNOWN;
	w->tall = 1.0e9f;
	return w;
}

void pt_water_destroy(pt_water_t *w)
{
	if (!w)
		return;
	free(w->h);
	free(w->u);
	free(w->v);
	free(w->fx);
	free(w->fy);
	free(w->depth);
	free(w->current);
	free(w->tmp);
	free(w->mark);
	free(w->stir);
	free(w->waves);
	free(w->caustics);
	free(w->open);
	free(w->foam);
	free(w->old);
	free(w->shore);
	free(w->froth);
	free(w);
}

int pt_water_width(const pt_water_t *w) { return w->width; }
int pt_water_height(const pt_water_t *w) { return w->height; }
float pt_water_cell(const pt_water_t *w) { return w->cell; }

void pt_water_disturb(pt_water_t *w, float x, float y, float radius, float amount)
{
	const float cx = (x - w->min_x) / w->cell, cy = (y - w->min_y) / w->cell;
	float r = radius / w->cell;
	int x0, y0, x1, y1, ix, iy;

	if (r < 1.5f)
		r = 1.5f;
	x0 = (int)floorf(cx - r); x1 = (int)ceilf(cx + r);
	y0 = (int)floorf(cy - r); y1 = (int)ceilf(cy + r);
	if (x0 < 0) x0 = 0;
	if (y0 < 0) y0 = 0;
	if (x1 > w->width - 1) x1 = w->width - 1;
	if (y1 > w->height - 1) y1 = w->height - 1;
	w->touched = 1;

	for (iy = y0; iy <= y1; iy++)
	{
		for (ix = x0; ix <= x1; ix++)
		{
			const float dx = ix + 0.5f - cx, dy = iy + 0.5f - cy;
			const float d = sqrtf(dx * dx + dy * dy) / r;
			if (d < 1.0f && w->open[(size_t)iy * w->width + ix])
			{
				/* a smooth dent, so it sends out a clean ring */
				const float fall = 0.5f * (1.0f + cosf(d * 3.14159265f));
				w->h[(size_t)iy * w->width + ix] -= amount * fall;
			}
		}
	}
}

void pt_water_foaming(pt_water_t *w, float amount, float life, float shore)
{
	if (amount > 0.0f && !w->foam)
	{
		w->foam = (float *)calloc((size_t)w->width * w->height, sizeof(float));
		w->old = (float *)calloc((size_t)w->width * w->height, sizeof(float));
		if (!w->foam || !w->old)
		{
			free(w->foam);
			free(w->old);
			w->foam = w->old = NULL;
		}
	}
	if ((w->foam && amount > 0.0f ? amount : 0.0f) != w->foaming || (shore > 0.0f ? shore : 0.0f) != w->shore_amount)
		w->touched = 1;		/* its pictures are not what they were */
	if (w->foam && amount <= 0.0f && w->foaming > 0.0f)
	{
		/* turned off: what there was goes at once */
		memset(w->foam, 0, (size_t)w->width * w->height * sizeof(float));
		memset(w->old, 0, (size_t)w->width * w->height * sizeof(float));
		w->foamy = 0;
		w->sprays = 0;
	}
	w->foaming = w->foam && amount > 0.0f ? amount : 0.0f;
	w->foam_life = life > 0.05f ? life : 0.05f;
	w->shore_amount = shore > 0.0f ? shore : 0.0f;
}

void pt_water_churn(pt_water_t *w, float x, float y, float radius, float amount)
{
	const float cx = (x - w->min_x) / w->cell, cy = (y - w->min_y) / w->cell;
	float r = radius / w->cell;
	int x0, y0, x1, y1, ix, iy;

	if (w->foaming <= 0.0f)
		return;
	w->touched = 1;
	if (r < 1.5f)
		r = 1.5f;
	x0 = (int)floorf(cx - r); x1 = (int)ceilf(cx + r);
	y0 = (int)floorf(cy - r); y1 = (int)ceilf(cy + r);
	if (x0 < 0) x0 = 0;
	if (y0 < 0) y0 = 0;
	if (x1 > w->width - 1) x1 = w->width - 1;
	if (y1 > w->height - 1) y1 = w->height - 1;

	for (iy = y0; iy <= y1; iy++)
	{
		for (ix = x0; ix <= x1; ix++)
		{
			const float dx = ix + 0.5f - cx, dy = iy + 0.5f - cy;
			const float d = sqrtf(dx * dx + dy * dy) / r;
			if (d < 1.0f && w->open[(size_t)iy * w->width + ix])
				froth(w, (size_t)iy * w->width + ix, amount * 0.5f * (1.0f + cosf(d * 3.14159265f)), 0);
		}
	}
}

/* a neighbour of a cell whose own value is self: the edge of the rectangle
   and cells without liquid act as walls, which waves bounce off */
static float at(const pt_water_t *w, const float *f, int x, int y, float self)
{
	if (x < 0 || y < 0 || x >= w->width || y >= w->height || !w->open[(size_t)y * w->width + x])
		return self;
	return f[(size_t)y * w->width + x];
}

/* the cells a triangle may cover, and its edges as lines:
   distance = e[0]*x + e[1]*y + e[2], positive inside */
typedef struct
{
	float	e[3][3];
	int		x0, y0, x1, y1;
} span_t;

static int span_of(const pt_water_t *w, const float *a, const float *b, const float *c, span_t *s)
{
	const float *p[3];
	float lo[2], hi[2], area;
	int i;

	p[0] = a; p[1] = b; p[2] = c;
	area = (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0]);
	if (area == 0.0f)
		return 0;
	for (i = 0; i < 3; i++)
	{
		const float *from = p[i], *to = p[(i + 1) % 3];
		float nx = -(to[1] - from[1]), ny = to[0] - from[0];
		const float len = sqrtf(nx * nx + ny * ny);
		if (len == 0.0f)
			return 0;
		if (area < 0.0f) { nx = -nx; ny = -ny; }
		s->e[i][0] = nx / len;
		s->e[i][1] = ny / len;
		s->e[i][2] = -(s->e[i][0] * from[0] + s->e[i][1] * from[1]);
	}
	for (i = 0; i < 2; i++)
	{
		lo[i] = a[i] < b[i] ? (a[i] < c[i] ? a[i] : c[i]) : (b[i] < c[i] ? b[i] : c[i]);
		hi[i] = a[i] > b[i] ? (a[i] > c[i] ? a[i] : c[i]) : (b[i] > c[i] ? b[i] : c[i]);
	}
	s->x0 = (int)floorf((lo[0] - w->min_x) / w->cell) - 1; s->x1 = (int)ceilf((hi[0] - w->min_x) / w->cell) + 1;
	s->y0 = (int)floorf((lo[1] - w->min_y) / w->cell) - 1; s->y1 = (int)ceilf((hi[1] - w->min_y) / w->cell) + 1;
	if (s->x0 < 0) s->x0 = 0;
	if (s->y0 < 0) s->y0 = 0;
	if (s->x1 > w->width - 1) s->x1 = w->width - 1;
	if (s->y1 > w->height - 1) s->y1 = w->height - 1;
	return 1;
}

/* a cell counts if the triangle comes within half a cell of its middle,
   so that neighbouring triangles leave no gaps between them */
static int span_has(const span_t *s, const pt_water_t *w, int x, int y, float *cx, float *cy)
{
	int i;

	*cx = w->min_x + (x + 0.5f) * w->cell;
	*cy = w->min_y + (y + 0.5f) * w->cell;
	for (i = 0; i < 3; i++)
		if (s->e[i][0] * *cx + s->e[i][1] * *cy + s->e[i][2] < -0.5f * w->cell)
			return 0;
	return 1;
}

void pt_water_cover(pt_water_t *w, const float a[2], const float b[2], const float c[2])
{
	span_t s;
	float cx, cy;
	int x, y;

	if (!w->covered)
	{
		memset(w->open, 0, (size_t)w->width * w->height);
		w->covered = 1;
	}
	if (!span_of(w, a, b, c, &s))
		return;
	for (y = s.y0; y <= s.y1; y++)
		for (x = s.x0; x <= s.x1; x++)
			if (span_has(&s, w, x, y, &cx, &cy))
				w->open[(size_t)y * w->width + x] = 1;
}

void pt_water_bed(pt_water_t *w, const float a[3], const float b[3], const float c[3])
{
	span_t s;
	/* the plane of the triangle: z = a[2] + sx * (x - a[0]) + sy * (y - a[1]) */
	const float nx = (b[1] - a[1]) * (c[2] - a[2]) - (b[2] - a[2]) * (c[1] - a[1]);
	const float ny = (b[2] - a[2]) * (c[0] - a[0]) - (b[0] - a[0]) * (c[2] - a[2]);
	const float nz = (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0]);
	float lo = a[2], hi = a[2], cx, cy, z;
	size_t i;
	int x, y;

	if (!w->sounded)
	{
		/* from now on the depth is known wherever something lies below */
		for (i = 0; i < (size_t)w->width * w->height; i++)
			w->depth[i] = -1.0f;
		w->sounded = 1;
		w->deepest = 0.0f;		/* found again at the next step */
	}
	if (nz == 0.0f || !span_of(w, a, b, c, &s))
		return;
	if (b[2] < lo) lo = b[2];
	if (c[2] < lo) lo = c[2];
	if (b[2] > hi) hi = b[2];
	if (c[2] > hi) hi = c[2];

	for (y = s.y0; y <= s.y1; y++)
	{
		for (x = s.x0; x <= s.x1; x++)
		{
			if (!span_has(&s, w, x, y, &cx, &cy))
				continue;
			z = a[2] - (nx * (cx - a[0]) + ny * (cy - a[1])) / nz;
			if (z < lo) z = lo;
			if (z > hi) z = hi;
			/* the shallowest thing under the surface is the bed */
			i = (size_t)y * w->width + x;
			if (z < -0.25f && (w->depth[i] < 0.0f || -z < w->depth[i]))
				w->depth[i] = -z;
		}
	}
}

void pt_water_current(pt_water_t *w, const float a[2], const float b[2], const float c[2], float vx, float vy)
{
	span_t s;
	float cx, cy;
	int x, y;

	if (!w->current)
		w->current = (float *)calloc((size_t)w->width * w->height * 2, sizeof(float));
	if (!w->current || !span_of(w, a, b, c, &s))
		return;
	for (y = s.y0; y <= s.y1; y++)
	{
		for (x = s.x0; x <= s.x1; x++)
		{
			if (!span_has(&s, w, x, y, &cx, &cy))
				continue;
			w->current[((size_t)y * w->width + x) * 2] = vx;
			w->current[((size_t)y * w->width + x) * 2 + 1] = vy;
		}
	}
}

void pt_water_move(pt_water_t *w, float x, float y, float radius, float vx, float vy, float amount, float dt)
{
	const float cx = (x - w->min_x) / w->cell, cy = (y - w->min_y) / w->cell;
	const float pi = 3.14159265f;
	float r = radius / w->cell, drag = 1.0f * dt;
	int x0, y0, x1, y1, ix, iy;

	w->touched = 1;
	if (r < 1.5f)
		r = 1.5f;
	if (drag > 1.0f)
		drag = 1.0f;
	x0 = (int)floorf(cx - r); x1 = (int)ceilf(cx + r);
	y0 = (int)floorf(cy - r); y1 = (int)ceilf(cy + r);
	if (x0 < 0) x0 = 0;
	if (y0 < 0) y0 = 0;
	if (x1 > w->width - 1) x1 = w->width - 1;
	if (y1 > w->height - 1) y1 = w->height - 1;

	for (iy = y0; iy <= y1; iy++)
	{
		for (ix = x0; ix <= x1; ix++)
		{
			const size_t i = (size_t)iy * w->width + ix;
			const float dx = ix + 0.5f - cx, dy = iy + 0.5f - cy;
			const float dist = sqrtf(dx * dx + dy * dy), d = dist / r;
			float fall, along;

			if (d >= 1.0f || !w->open[i])
				continue;
			fall = 0.5f * (1.0f + cosf(d * pi));

			/* It is a smooth hump of liquid's worth of something, and it
			   has moved: there is that much more of it ahead of where it
			   was and that much less behind, and the liquid makes room. So
			   the surface rises in front and sinks in the wake, and nothing
			   is added to the pool or taken from it. */
			if (dist > 0.0f)
			{
				along = (vx * dx + vy * dy) / (dist * w->cell);
				w->h[i] += amount * dt * along * 0.5f * pi * sinf(d * pi) / r;
			}

			/* and it drags along what it moves through: the top of the
			   liquid, which is the less of all of it the deeper it is */
			fall *= drag * (w->depth[i] > 16.0f ? 16.0f / w->depth[i] : 1.0f);
			if (ix > 0 && w->open[i - 1])
				w->u[(size_t)iy * (w->width + 1) + ix] += (vx - w->u[(size_t)iy * (w->width + 1) + ix]) * fall;
			if (iy > 0 && w->open[i - w->width])
				w->v[i] += (vy - w->v[i]) * fall;
		}
	}
}

/* how deep the liquid stands in a cell just now */
static float standing(const pt_water_t *w, size_t i)
{
	const float d = (w->depth[i] > DEPTH_MAX ? DEPTH_MAX : w->depth[i]) + w->h[i];
	return d > 0.0f ? d : 0.0f;
}

/* a field of across by down values at a place between them, held to its edges */
static float upstream(const float *f, int across, int down, float x, float y)
{
	int x0, y0;
	float ax, ay;

	if (x < 0.0f) x = 0.0f;
	if (y < 0.0f) y = 0.0f;
	if (x > across - 1) x = (float)(across - 1);
	if (y > down - 1) y = (float)(down - 1);
	x0 = (int)x; y0 = (int)y;
	if (x0 > across - 2) x0 = across - 2;
	if (y0 > down - 2) y0 = down - 2;
	ax = x - x0; ay = y - y0;
	return (f[(size_t)y0 * across + x0] * (1.0f - ax) + f[(size_t)y0 * across + x0 + 1] * ax) * (1.0f - ay)
		+ (f[(size_t)(y0 + 1) * across + x0] * (1.0f - ax) + f[(size_t)(y0 + 1) * across + x0 + 1] * ax) * ay;
}

/*
The shallow water equations on a grid: each cell holds how high the surface
stands, each face between two cells how fast the liquid crosses it. A slope
in the surface sets the liquid moving down it, and what crosses a face is
that speed times the depth of the cell it comes out of. So waves run at the
square root of gravity times depth: slower over a shallow bed, where they
bunch up and grow, and they turn towards it.
*/
static void step_once(pt_water_t *w, float dt, float gravity, float damping)
{
	const int width = w->width, height = w->height;
	const float keep = powf(1.0f - damping, dt);
	const float pull = gravity * dt / w->cell, share = dt / w->cell;
	/* nothing may cross more than a good part of a cell in a step */
	const float fastest = 0.4f * w->cell / dt;
	/* a face of liquid steeper than this topples */
	const float steepest = 0.75f * w->cell;
	double sum = 0.0;
	size_t wet = 0;
	int x, y;

	for (y = 0; y < height; y++)
	{
		for (x = 0; x <= width; x++)
		{
			const size_t f = (size_t)y * (width + 1) + x, i = (size_t)y * width + x;
			float speed = 0.0f;
			if (x > 0 && x < width && w->open[i] && w->open[i - 1])
			{
				speed = (w->u[f] - pull * (w->h[i] - w->h[i - 1])) * keep;
				if (speed > fastest) speed = fastest;
				if (speed < -fastest) speed = -fastest;
			}
			w->u[f] = speed;
			w->fx[f] = speed == 0.0f ? 0.0f : speed * standing(w, speed > 0.0f ? i - 1 : i);
		}
	}
	for (y = 0; y <= height; y++)
	{
		for (x = 0; x < width; x++)
		{
			const size_t i = (size_t)y * width + x;
			float speed = 0.0f;
			if (y > 0 && y < height && w->open[i] && w->open[i - width])
			{
				speed = (w->v[i] - pull * (w->h[i] - w->h[i - width])) * keep;
				if (speed > fastest) speed = fastest;
				if (speed < -fastest) speed = -fastest;
			}
			w->v[i] = speed;
			w->fy[i] = speed == 0.0f ? 0.0f : speed * standing(w, speed > 0.0f ? i - width : i);
		}
	}

	/* no cell gives more than it holds: tmp is the share of what it would
	   send out that it can */
	for (y = 0; y < height; y++)
	{
		for (x = 0; x < width; x++)
		{
			const size_t i = (size_t)y * width + x, f = (size_t)y * (width + 1) + x;
			float out = 0.0f;
			if (w->fx[f] < 0.0f) out -= w->fx[f];
			if (w->fx[f + 1] > 0.0f) out += w->fx[f + 1];
			if (w->fy[i] < 0.0f) out -= w->fy[i];
			if (w->fy[i + width] > 0.0f) out += w->fy[i + width];
			out *= share;
			w->tmp[i] = out > standing(w, i) ? standing(w, i) / out : 1.0f;
		}
	}
	for (y = 0; y < height; y++)
	{
		for (x = 0; x < width; x++)
		{
			const size_t i = (size_t)y * width + x, f = (size_t)y * (width + 1) + x;
			const float left = w->fx[f] * (w->fx[f] > 0.0f ? (x > 0 ? w->tmp[i - 1] : 1.0f) : w->tmp[i]);
			const float right = w->fx[f + 1] * (w->fx[f + 1] > 0.0f ? w->tmp[i] : (x < width - 1 ? w->tmp[i + 1] : 1.0f));
			const float below = w->fy[i] * (w->fy[i] > 0.0f ? (y > 0 ? w->tmp[i - width] : 1.0f) : w->tmp[i]);
			const float above = w->fy[i + width] * (w->fy[i + width] > 0.0f ? w->tmp[i] : (y < height - 1 ? w->tmp[i + width] : 1.0f));
			if (!w->open[i])
				continue;
			w->h[i] -= (right - left + above - below) * share;
			/* whatever went wrong, it stops here */
			if (!(w->h[i] > -DEPTH_MAX))
				w->h[i] = 0.0f;
			if (w->h[i] > 4.0f * PT_WATER_HEIGHT_MAX)
				w->h[i] = 4.0f * PT_WATER_HEIGHT_MAX;
		}
	}

	/* Where a wave has grown too steep to stand it breaks: the top of the
	   step goes to its foot. Each pair of cells is settled once, so
	   nothing is made or lost. */
	for (y = 0; y < height; y++)
	{
		for (x = 0; x < width; x++)
		{
			const size_t i = (size_t)y * width + x;
			float over;
			if (!w->open[i])
				continue;
			if (x > 0 && w->open[i - 1])
			{
				over = w->h[i] - w->h[i - 1];
				over = over > steepest ? over - steepest : (over < -steepest ? over + steepest : 0.0f);
				w->h[i] -= 0.25f * over;
				w->h[i - 1] += 0.25f * over;
				if (over != 0.0f && w->foaming > 0.0f)
					froth(w, over > 0.0f ? i : i - 1, fabsf(over) * 0.5f, 1);
			}
			if (y > 0 && w->open[i - width])
			{
				over = w->h[i] - w->h[i - width];
				over = over > steepest ? over - steepest : (over < -steepest ? over + steepest : 0.0f);
				w->h[i] -= 0.25f * over;
				w->h[i - width] += 0.25f * over;
				if (over != 0.0f && w->foaming > 0.0f)
					froth(w, over > 0.0f ? i : i - width, fabsf(over) * 0.5f, 1);
			}
		}
	}

	/* A stream carries the waves along with it: the shape of the surface
	   and the flow that goes with it. What is here now was upstream a step
	   ago. fx and fy are free to hold the old flow. */
	if (w->current)
	{
		memcpy(w->tmp, w->h, (size_t)width * height * sizeof(float));
		memcpy(w->fx, w->u, (size_t)(width + 1) * height * sizeof(float));
		memcpy(w->fy, w->v, (size_t)width * (height + 1) * sizeof(float));
		for (y = 0; y < height; y++)
		{
			for (x = 0; x < width; x++)
			{
				const size_t i = (size_t)y * width + x;
				const float cx = w->current[i * 2] * share, cy = w->current[i * 2 + 1] * share;
				if (!w->open[i] || (cx == 0.0f && cy == 0.0f))
					continue;
				w->h[i] = upstream(w->tmp, width, height, x - cx, y - cy);
				/* the faces on the low side of the cell, where both sides of them are in the stream */
				if (x > 0 && w->open[i - 1] && (w->current[(i - 1) * 2] != 0.0f || w->current[(i - 1) * 2 + 1] != 0.0f))
					w->u[(size_t)y * (width + 1) + x] = upstream(w->fx, width + 1, height, x - cx, y - cy);
				if (y > 0 && w->open[i - width] && (w->current[(i - width) * 2] != 0.0f || w->current[(i - width) * 2 + 1] != 0.0f))
					w->v[i] = upstream(w->fy, width, height + 1, x - cx, y - cy);
			}
		}
	}

	/* What a splash knocks out of the surface comes back, and a pool finds
	   its level again: the whole surface drifts towards it. */
	for (y = 0; y < height; y++)
		for (x = 0; x < width; x++)
			if (w->open[(size_t)y * width + x])
			{
				sum += w->h[(size_t)y * width + x];
				wet++;
			}
	if (wet)
	{
		const float back = (float)(sum / (double)wet) * (1.0f - powf(0.5f, dt));
		for (y = 0; y < height; y++)
			for (x = 0; x < width; x++)
				if (w->open[(size_t)y * width + x])
					w->h[(size_t)y * width + x] -= back;
	}
}

/*
Froth: it forms where the surface stands too steep to hold together and
where a stream runs into something, rides along on the liquid under it, and
dies away. Once a call of pt_water_step is enough: it is slow next to waves.
*/
static void foam_step(pt_water_t *w, float dt)
{
	const int width = w->width, height = w->height;
	const float keep = powf(0.5f, dt / w->foam_life), share = dt / w->cell;
	/* by the time a quarter of it is left it is old */
	const float ages = dt / (2.0f * w->foam_life);
	int x, y, any = 0;

	/* a surface that nowhere stands half a step from level has no step in
	   it worth looking for: most pools, most of the time */
	for (y = w->current || w->tall >= 0.5f * FOAM_STEEP * w->cell ? 0 : height; y < height; y++)
	{
		for (x = 0; x < width; x++)
		{
			const size_t i = (size_t)y * width + x;
			float steep = 0.0f, d;
			if (!w->open[i])
				continue;
			if (x > 0 && w->open[i - 1] && (d = fabsf(w->h[i] - w->h[i - 1])) > steep) steep = d;
			if (y > 0 && w->open[i - width] && (d = fabsf(w->h[i] - w->h[i - width])) > steep) steep = d;
			steep = steep / w->cell - FOAM_STEEP;
			if (steep > 0.0f)
				froth(w, i, steep * 25.0f * dt, 0);
			if (w->current)
			{
				/* a stream that ends, at a bank or in still water, piles up there */
				const float cx = w->current[i * 2], cy = w->current[i * 2 + 1];
				const int nx = x + (cx > 0.0f) - (cx < 0.0f), ny = y + (cy > 0.0f) - (cy < 0.0f);
				if (cx != 0.0f || cy != 0.0f)
				{
					/* against a bank it stays where it is made, see below; in
					   still water it is left where the stream lets go of it */
					const size_t n = (size_t)ny * width + nx;
					if (nx < 0 || ny < 0 || nx >= width || ny >= height || !w->open[n])
						froth(w, i, sqrtf(cx * cx + cy * cy) * 0.005f * dt, 0);
					else if (w->current[n * 2] == 0.0f && w->current[n * 2 + 1] == 0.0f)
						froth(w, n, sqrtf(cx * cx + cy * cy) * 0.005f * dt, 0);
				}
			}
		}
	}
	if (!w->foamy)
		return;

	/* what is here now was upstream a moment ago; fx is free to hold the other half of it */
	memcpy(w->tmp, w->foam, (size_t)width * height * sizeof(float));
	memcpy(w->fx, w->old, (size_t)width * height * sizeof(float));
	for (y = 0; y < height; y++)
	{
		for (x = 0; x < width; x++)
		{
			const size_t i = (size_t)y * width + x, f = (size_t)y * (width + 1) + x;
			float vx, vy;
			if (!w->open[i])
				continue;
			vx = 0.5f * (w->u[f] + w->u[f + 1]);
			vy = 0.5f * (w->v[i] + w->v[i + width]);
			if (w->current)
			{
				/* a stream takes it as far as the bank and no further */
				const float cx = w->current[i * 2], cy = w->current[i * 2 + 1];
				const int nx = x + (cx > 0.0f) - (cx < 0.0f), ny = y + (cy > 0.0f) - (cy < 0.0f);
				if (cx != 0.0f && nx >= 0 && nx < width && w->open[(size_t)y * width + nx])
					vx += cx;
				if (cy != 0.0f && ny >= 0 && ny < height && w->open[(size_t)ny * width + x])
					vy += cy;
			}
			if (vx != 0.0f || vy != 0.0f)
			{
				w->foam[i] = upstream(w->tmp, width, height, x - vx * share, y - vy * share);
				w->old[i] = upstream(w->fx, width, height, x - vx * share, y - vy * share);
			}
			w->old[i] = (w->old[i] + w->foam[i] * ages) * keep;
			w->foam[i] *= keep;
			if (w->old[i] > w->foam[i])
				w->old[i] = w->foam[i];
			if (w->foam[i] > 1.0f / 512.0f)
				any = 1;
			else
				w->foam[i] = w->old[i] = 0.0f;
		}
	}
	w->foamy = any;
}

void pt_water_step(pt_water_t *w, float dt, float gravity, float damping)
{
	/* The scheme is stable while a wave crosses less than about 0.7 of a
	   cell per step, so the step is as small as the deepest part needs. */
	float step;
	int guard = 0;

	if (!w->deepest)
	{
		/* the bed has been sounded: settle what is known of it */
		size_t i;
		for (i = 0; i < (size_t)w->width * w->height; i++)
		{
			if (w->depth[i] < 0.0f)
				w->depth[i] = DEPTH_UNKNOWN;
			if (w->depth[i] < DEPTH_MIN)
				w->depth[i] = DEPTH_MIN;
			if (w->open[i] && w->depth[i] > w->deepest)
				w->deepest = w->depth[i] > DEPTH_MAX ? DEPTH_MAX : w->depth[i];
		}
		if (!w->deepest)
			w->deepest = DEPTH_MIN;
	}

	if (gravity < 1.0f)
		gravity = 1.0f;
	if (damping < 0.0f) damping = 0.0f;
	if (damping > 0.999f) damping = 0.999f;
	if (dt > 0.1f)
		dt = 0.1f;		/* after a hitch, do not try to catch up */
	step = 0.5f * w->cell / sqrtf(gravity * (w->deepest + PT_WATER_HEIGHT_MAX));
	if (step > 1.0f / 60.0f)
		step = 1.0f / 60.0f;
	/* Asked for less than that at a time, it moves by what it is asked
	   for: whoever calls more often than 60 times a second sees it move
	   each time, and not every other. */
	if (dt < step && w->leftover <= 0.0f)
		step = dt > 0.001f ? dt : 0.001f;

	w->leftover += dt;
	while (w->leftover >= step && guard++ < 32)
	{
		step_once(w, step, gravity, damping);
		w->leftover -= step;
	}
	if (guard >= 32 || w->leftover < 1.0e-6f)
		w->leftover = 0.0f;
	w->settling = 0;
	{
		/* What the liquid has stood against stays wet for a while. And
		   where waves are, or a stream, the surface is ruffled with ripples
		   too fine for the cells, which are a while in settling. */
		const float dried = WET_DRIES * dt, settled = STIR_SETTLES * dt;
		int x, y;

		for (y = 0; y < w->height; y++)
		{
			for (x = 0; x < w->width; x++)
			{
				const size_t i = (size_t)y * w->width + x;
				const float here = w->h[i];
				const float was = w->mark[i] - dried;
				float busy = (fabsf(at(w, w->h, x + 1, y, here) - at(w, w->h, x - 1, y, here))
					+ fabsf(at(w, w->h, x, y + 1, here) - at(w, w->h, x, y - 1, here))) * (STIR_SLOPE / w->cell)
					+ fabsf(here) * STIR_HEIGHT;

				if (w->current)
					busy += (fabsf(w->current[i * 2]) + fabsf(w->current[i * 2 + 1])) * STIR_STREAM;
				if (busy > 1.0f) busy = 1.0f;
				/* a mark still drying, or ripples still settling, keep it awake */
				if (w->open[i] && (w->mark[i] > here + HEIGHT_REST || w->stir[i] > busy + 0.01f))
					w->settling = 1;
				w->mark[i] = here > was ? here : was;
				if (busy < w->stir[i] - settled) busy = w->stir[i] - settled;
				w->stir[i] = busy;
			}
		}
	}
	if (w->foaming > 0.0f)
		foam_step(w, dt);

	/* Has it come to rest? Then it is put level, which nobody can tell
	   from where it was, and stays so until something touches it. */
	w->touched = 0;
	w->quiet = !w->foamy && !w->sprays && !w->settling;
	if (w->quiet)
	{
		const size_t cells = (size_t)w->width * w->height;
		const size_t us = (size_t)(w->width + 1) * w->height, vs = (size_t)w->width * (w->height + 1);
		size_t i;
		for (i = 0; i < cells && w->quiet; i++)
			if (w->open[i] && fabsf(w->h[i]) > HEIGHT_REST)
				w->quiet = 0;
		for (i = 0; i < us && w->quiet; i++)
			if (fabsf(w->u[i]) > FLOW_REST)
				w->quiet = 0;
		for (i = 0; i < vs && w->quiet; i++)
			if (fabsf(w->v[i]) > FLOW_REST)
				w->quiet = 0;
		if (w->quiet)
		{
			memset(w->h, 0, cells * sizeof(float));
			memset(w->u, 0, us * sizeof(float));
			memset(w->v, 0, vs * sizeof(float));
			w->leftover = 0.0f;
		}
	}
}

int pt_water_still(const pt_water_t *w)
{
	return w->quiet && !w->touched;
}

int pt_water_spray(pt_water_t *w, const float **at)
{
	const int count = w->sprays;

	*at = w->spray;
	w->sprays = 0;
	return count;
}

static uint32_t byte_of(float v)
{
	if (v < 0.0f) v = 0.0f;
	if (v > 1.0f) v = 1.0f;
	return (uint32_t)(v * 255.0f + 0.5f);
}

const uint32_t *pt_water_waves(pt_water_t *w, float wave_scale)
{
	const float inv = wave_scale / (2.0f * w->cell);
	float reach = 0.0f;
	int x, y;

	for (y = 0; y < w->height; y++)
	{
		for (x = 0; x < w->width; x++)
		{
			const float here = w->h[(size_t)y * w->width + x];
			const float sx = (at(w, w->h, x + 1, y, here) - at(w, w->h, x - 1, y, here)) * inv;
			const float sy = (at(w, w->h, x, y + 1, here) - at(w, w->h, x, y - 1, here)) * inv;
			float height = here * wave_scale;
			uint32_t stored;

			if (height < -PT_WATER_HEIGHT_MAX) height = -PT_WATER_HEIGHT_MAX;
			if (height > PT_WATER_HEIGHT_MAX) height = PT_WATER_HEIGHT_MAX;
			if (fabsf(height) > reach)
				reach = fabsf(height);
			stored = (uint32_t)((height / (2.0f * PT_WATER_HEIGHT_MAX) + 0.5f) * 65535.0f + 0.5f);
			w->waves[(size_t)y * w->width + x] =
				byte_of(sx * PT_WATER_SLOPE_SCALE + 0.5f)
				| (byte_of(sy * PT_WATER_SLOPE_SCALE + 0.5f) << 8)
				| ((stored >> 8) << 16)
				| ((stored & 0xff) << 24);
		}
	}
	w->reach = reach;
	w->shown = wave_scale;
	w->tall = wave_scale > 0.0f ? reach / wave_scale : 1.0e9f;
	return w->waves;
}

float pt_water_height_at(const pt_water_t *w, float x, float y, float wave_scale, int *covered)
{
	/* between the middles of the cells, and level with the outermost beyond them */
	float fx = (x - w->min_x) / w->cell - 0.5f, fy = (y - w->min_y) / w->cell - 0.5f, ax, ay, height;
	const int cx = (int)floorf((x - w->min_x) / w->cell), cy = (int)floorf((y - w->min_y) / w->cell);
	const int inside = cx >= 0 && cy >= 0 && cx < w->width && cy < w->height;
	const float *row;
	int x0, y0;

	if (covered)
		*covered = inside && w->open[(size_t)cy * w->width + cx];
	if (!inside)
		return 0.0f;
	if (fx < 0.0f) fx = 0.0f;
	if (fy < 0.0f) fy = 0.0f;
	if (fx > w->width - 1) fx = (float)(w->width - 1);
	if (fy > w->height - 1) fy = (float)(w->height - 1);
	x0 = (int)fx; y0 = (int)fy;
	if (x0 > w->width - 2) x0 = w->width - 2;
	if (y0 > w->height - 2) y0 = w->height - 2;
	ax = fx - x0; ay = fy - y0;
	row = &w->h[(size_t)y0 * w->width + x0];
	height = ((row[0] * (1.0f - ax) + row[1] * ax) * (1.0f - ay)
		+ (row[w->width] * (1.0f - ax) + row[w->width + 1] * ax) * ay) * wave_scale;
	if (height < -PT_WATER_HEIGHT_MAX) height = -PT_WATER_HEIGHT_MAX;
	if (height > PT_WATER_HEIGHT_MAX) height = PT_WATER_HEIGHT_MAX;
	return height;
}

float pt_water_reach(const pt_water_t *w) { return w->reach; }

/* how high the liquid has lately stood here or in a cell next to this one,
   or less than nothing where there is no liquid in any of them */
static float mark_near(const pt_water_t *w, int x, int y)
{
	float most = -1.0e9f;
	int dx, dy;

	for (dy = -1; dy <= 1; dy++)
		for (dx = -1; dx <= 1; dx++)
			if (x + dx >= 0 && y + dy >= 0 && x + dx < w->width && y + dy < w->height
				&& w->open[(size_t)(y + dy) * w->width + x + dx]
				&& w->mark[(size_t)(y + dy) * w->width + x + dx] > most)
				most = w->mark[(size_t)(y + dy) * w->width + x + dx];
	return most;
}

/* a curvature as a byte: see PT_WATER_CURVE_MAX */
static uint32_t curve_byte(float c)
{
	float s = sqrtf(fabsf(c) / PT_WATER_CURVE_MAX);

	if (s > 1.0f) s = 1.0f;
	return (uint32_t)(128.5f + (c < 0.0f ? -127.0f : 127.0f) * s);
}

const uint32_t *pt_water_caustics(pt_water_t *w, float strength)
{
	/*
	Where the surface bulges it gathers the light passing through, as a
	lens does, and where it is hollow it spreads it. How much, and how far
	below, goes by how sharply it curves, which is all that is worked out
	here: whoever lights something through the surface knows how far the
	light has to go and does the rest.
	*/
	const float scale = strength * w->shown / (w->cell * w->cell);
	int x, y;

	/* smoothed a little first: light does not come from one point */
	for (y = 0; y < w->height; y++)
	{
		for (x = 0; x < w->width; x++)
		{
			const float here = w->h[(size_t)y * w->width + x];
			w->tmp[(size_t)y * w->width + x] = (at(w, w->h, x - 1, y, here) + at(w, w->h, x + 1, y, here)
				+ at(w, w->h, x, y - 1, here) + at(w, w->h, x, y + 1, here) + 4.0f * here) * 0.125f;
		}
	}

	for (y = 0; y < w->height; y++)
	{
		for (x = 0; x < w->width; x++)
		{
			const float here = w->tmp[(size_t)y * w->width + x];
			const float left = at(w, w->tmp, x - 1, y, here), right = at(w, w->tmp, x + 1, y, here);
			const float below = at(w, w->tmp, x, y - 1, here), above = at(w, w->tmp, x, y + 1, here);
			const float xx = (left + right - 2.0f * here) * scale;
			const float yy = (below + above - 2.0f * here) * scale;
			const float stir = w->open[(size_t)y * w->width + x]
				? (STIR_REST + (1.0f - STIR_REST) * w->stir[(size_t)y * w->width + x]) * strength * (1.0f / PT_WATER_RIPPLE_MAX) : 0.0f;
			const float mark = mark_near(w, x, y) * w->shown / PT_WATER_HEIGHT_MAX;
			uint32_t wet = 0;

			if (mark > -1.0e8f)
				wet = PT_WATER_WET + (uint32_t)((mark < 0.0f ? 0.0f : mark > 1.0f ? 1.0f : mark) * (255 - PT_WATER_WET) + 0.5f);
			w->caustics[(size_t)y * w->width + x] = curve_byte(xx) | (curve_byte(yy) << 8) | (byte_of(stir) << 16) | (wet << 24);
		}
	}
	return w->caustics;
}

/*
Liquid froths of itself where it laps against a bank: how much, for each
cell, from how near the bank is.
*/
static void find_shore(pt_water_t *w)
{
	const int width = w->width, height = w->height;
	int x, y, dx, dy;

	w->shore = (float *)calloc((size_t)width * height, sizeof(float));
	if (!w->shore)
		return;
	for (y = 0; y < height; y++)
	{
		for (x = 0; x < width; x++)
		{
			const size_t i = (size_t)y * width + x;
			float most = 0.0f;
			if (!w->open[i])
				continue;
			for (dy = -2; dy <= 2; dy++)
			{
				for (dx = -2; dx <= 2; dx++)
				{
					const int nx = x + dx, ny = y + dy;
					if (nx < 0 || ny < 0 || nx >= width || ny >= height || !w->open[(size_t)ny * width + nx])
					{
						/* right against it, or a cell or two away */
						const int far2 = dx * dx + dy * dy;
						const float by = far2 <= 1 ? 1.0f : (far2 <= 2 ? 0.8f : (far2 <= 4 ? 0.55f : 0.25f));
						if (by > most)
							most = by;
					}
				}
			}
			w->shore[i] = most;
		}
	}
}

const uint32_t *pt_water_foam(pt_water_t *w)
{
	const int width = w->width, height = w->height;
	int x, y;

	if (!w->froth)
		w->froth = (uint32_t *)calloc((size_t)width * height, sizeof(uint32_t));
	if (!w->froth)
		return NULL;
	if (!w->shore && w->shore_amount > 0.0f)
		find_shore(w);

	for (y = 0; y < height; y++)
	{
		for (x = 0; x < width; x++)
		{
			const size_t i = (size_t)y * width + x, f = (size_t)y * (width + 1) + x;
			float cover = 0.0f, age = 0.0f, vx, vy;

			if (!w->open[i] || w->foaming <= 0.0f)
			{
				w->froth[i] = 0x80800000u;
				continue;
			}
			if (w->foam[i] > 0.0f)
			{
				cover = w->foam[i];
				age = w->old[i] / w->foam[i];
			}
			if (w->shore && w->shore_amount > 0.0f && w->shore[i] > 0.0f)
			{
				/* a little always, and more as the liquid rises against the bank */
				float lap = 1.0f + (w->h[i] > 0.0f ? w->h[i] * 1.2f : 0.0f) + fabsf(w->h[i]) * 0.4f;
				if (lap > 1.8f)
					lap = 1.8f;
				lap *= w->shore[i] * w->shore_amount;
				if (lap > cover)
				{
					cover = lap;
					age = 0.75f;	/* what is always there is never fresh */
				}
			}
			vx = 0.5f * (w->u[f] + w->u[f + 1]);
			vy = 0.5f * (w->v[i] + w->v[i + width]);
			if (w->current)
			{
				vx += w->current[i * 2];
				vy += w->current[i * 2 + 1];
			}
			w->froth[i] = byte_of(cover) | (byte_of(age) << 8)
				| (byte_of(vx * (1.0f / 255.0f) + 128.0f / 255.0f) << 16)
				| (byte_of(vy * (1.0f / 255.0f) + 128.0f / 255.0f) << 24);
		}
	}
	return w->froth;
}
