/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 Jonathan Ferguson */

#include "pt_png.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------ a byte buffer */

typedef struct
{
	uint8_t		*data;
	size_t		size, room;
	uint32_t	bits;		/* not yet written out, lowest first */
	int			numbits;
	int			failed;
} out_t;

static void put_byte(out_t *o, uint8_t b)
{
	if (o->size == o->room)
	{
		const size_t room = o->room ? o->room * 2 : 65536;
		uint8_t *data = (uint8_t *)realloc(o->data, room);
		if (!data)
		{
			o->failed = 1;
			return;
		}
		o->data = data;
		o->room = room;
	}
	o->data[o->size++] = b;
}

/* deflate packs everything from the low bit of each byte upwards */
static void put_bits(out_t *o, uint32_t value, int count)
{
	o->bits |= value << o->numbits;
	o->numbits += count;
	while (o->numbits >= 8)
	{
		put_byte(o, (uint8_t)o->bits);
		o->bits >>= 8;
		o->numbits -= 8;
	}
}

static void flush_bits(out_t *o)
{
	if (o->numbits)
		put_byte(o, (uint8_t)o->bits);
	o->bits = 0;
	o->numbits = 0;
}

/* ------------------------------------------------------------ Huffman codes */

#define MAX_SYMBOLS		288

/*
Code lengths for symbols used freq[] times each, none longer than limit.
Built the plain way, by joining the two rarest again and again; if that
comes out too deep the counts are evened out a little and it is done over.
*/
static void code_lengths(const uint32_t *freq_in, int num, int limit, uint8_t *length)
{
	uint32_t	freq[MAX_SYMBOLS];
	uint32_t	weight[MAX_SYMBOLS * 2];
	int			parent[MAX_SYMBOLS * 2];
	int			alive[MAX_SYMBOLS], num_alive;
	int			i, used = 0;

	for (i = 0; i < num; i++)
	{
		freq[i] = freq_in[i];
		if (freq[i])
			used++;
	}
	/* a code needs two symbols to be complete */
	for (i = 0; i < num && used < 2; i++)
	{
		if (!freq[i])
		{
			freq[i] = 1;
			used++;
		}
	}

	for (;;)
	{
		int nodes = num, deepest = 0;

		num_alive = 0;
		for (i = 0; i < num; i++)
		{
			weight[i] = freq[i];
			parent[i] = -1;
			if (freq[i])
				alive[num_alive++] = i;
		}

		while (num_alive > 1)
		{
			int a = 0, b = 1, k;

			/* the two lightest */
			if (weight[alive[b]] < weight[alive[a]])
			{
				a = 1;
				b = 0;
			}
			for (k = 2; k < num_alive; k++)
			{
				if (weight[alive[k]] < weight[alive[a]])
				{
					b = a;
					a = k;
				}
				else if (weight[alive[k]] < weight[alive[b]])
					b = k;
			}
			weight[nodes] = weight[alive[a]] + weight[alive[b]];
			parent[nodes] = -1;
			parent[alive[a]] = nodes;
			parent[alive[b]] = nodes;
			/* the new node takes one's place and the last takes the other's */
			alive[a] = nodes++;
			alive[b] = alive[--num_alive];
		}

		for (i = 0; i < num; i++)
		{
			int depth = 0, p;

			if (freq[i])
				for (p = parent[i]; p >= 0; p = parent[p])
					depth++;
			length[i] = (uint8_t)depth;
			if (depth > deepest)
				deepest = depth;
		}
		if (deepest <= limit)
			return;

		for (i = 0; i < num; i++)
			if (freq[i])
				freq[i] = (freq[i] + 1) / 2;
	}
}

/* the codes that go with the lengths, already turned round for put_bits */
static void make_codes(const uint8_t *length, int num, uint16_t *code)
{
	int			count[16] = {0};
	uint32_t	next[16];
	uint32_t	c = 0;
	int			i, bit;

	for (i = 0; i < num; i++)
		count[length[i]]++;
	count[0] = 0;
	for (i = 1; i < 16; i++)
	{
		c = (c + count[i - 1]) << 1;
		next[i] = c;
	}
	for (i = 0; i < num; i++)
	{
		uint32_t v, r = 0;

		if (!length[i])
		{
			code[i] = 0;
			continue;
		}
		v = next[length[i]]++;
		for (bit = 0; bit < length[i]; bit++)
			r |= ((v >> bit) & 1) << (length[i] - 1 - bit);
		code[i] = (uint16_t)r;
	}
}

/* ------------------------------------------------------------------ deflate */

static const uint16_t length_base[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59,
	67, 83, 99, 115, 131, 163, 195, 227, 258};
static const uint8_t length_extra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3,
	4, 4, 4, 4, 5, 5, 5, 5, 0};
static const uint16_t dist_base[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513,
	769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
static const uint8_t dist_extra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8,
	8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

static int length_symbol(int len)
{
	int s = 28;
	while (length_base[s] > len)
		s--;
	return s;
}

static int dist_symbol(int dist)
{
	int s = 29;
	while (dist_base[s] > dist)
		s--;
	return s;
}

/* a token is a byte as it stands, or a copy of something seen before */
#define TOKEN_COPY		0x80000000u
#define COPY(len, dist)	(TOKEN_COPY | ((uint32_t)(len) << 15) | (uint32_t)((dist) - 1))
#define COPY_LEN(t)		(int)(((t) >> 15) & 0x1ff)
#define COPY_DIST(t)	(int)(((t) & 0x7fff) + 1)

#define BLOCK_TOKENS	65536

static void write_block(out_t *o, const uint32_t *tokens, int count, int last)
{
	static const uint8_t order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
	uint32_t	lit_freq[286] = {0}, dist_freq[30] = {0}, cl_freq[19] = {0};
	uint8_t		lit_len[286], dist_len[30], cl_len[19];
	uint16_t	lit_code[286], dist_code[30], cl_code[19];
	int			i, num_lit, num_dist;

	for (i = 0; i < count; i++)
	{
		const uint32_t t = tokens[i];
		if (t & TOKEN_COPY)
		{
			lit_freq[257 + length_symbol(COPY_LEN(t))]++;
			dist_freq[dist_symbol(COPY_DIST(t))]++;
		}
		else
			lit_freq[t]++;
	}
	lit_freq[256] = 1;		/* the end of the block */

	code_lengths(lit_freq, 286, 15, lit_len);
	code_lengths(dist_freq, 30, 15, dist_len);
	make_codes(lit_len, 286, lit_code);
	make_codes(dist_len, 30, dist_code);

	for (num_lit = 286; num_lit > 257 && !lit_len[num_lit - 1]; num_lit--)
		;
	for (num_dist = 30; num_dist > 1 && !dist_len[num_dist - 1]; num_dist--)
		;

	/* the lengths are themselves sent with a code, each one spelt out */
	for (i = 0; i < num_lit; i++)
		cl_freq[lit_len[i]]++;
	for (i = 0; i < num_dist; i++)
		cl_freq[dist_len[i]]++;
	code_lengths(cl_freq, 19, 7, cl_len);
	make_codes(cl_len, 19, cl_code);

	put_bits(o, last ? 1 : 0, 1);
	put_bits(o, 2, 2);		/* its own codes */
	put_bits(o, (uint32_t)(num_lit - 257), 5);
	put_bits(o, (uint32_t)(num_dist - 1), 5);
	put_bits(o, 19 - 4, 4);
	for (i = 0; i < 19; i++)
		put_bits(o, cl_len[order[i]], 3);
	for (i = 0; i < num_lit; i++)
		put_bits(o, cl_code[lit_len[i]], cl_len[lit_len[i]]);
	for (i = 0; i < num_dist; i++)
		put_bits(o, cl_code[dist_len[i]], cl_len[dist_len[i]]);

	for (i = 0; i < count; i++)
	{
		const uint32_t t = tokens[i];
		if (t & TOKEN_COPY)
		{
			const int len = COPY_LEN(t), dist = COPY_DIST(t);
			const int ls = length_symbol(len), ds = dist_symbol(dist);

			put_bits(o, lit_code[257 + ls], lit_len[257 + ls]);
			if (length_extra[ls])
				put_bits(o, (uint32_t)(len - length_base[ls]), length_extra[ls]);
			put_bits(o, dist_code[ds], dist_len[ds]);
			if (dist_extra[ds])
				put_bits(o, (uint32_t)(dist - dist_base[ds]), dist_extra[ds]);
		}
		else
			put_bits(o, lit_code[t], lit_len[t]);
	}
	put_bits(o, lit_code[256], lit_len[256]);
}

#define HASH_BITS	15
#define HASH_SIZE	(1 << HASH_BITS)
#define WINDOW		32768
#define MAX_CHAIN	24

static uint32_t hash3(const uint8_t *p)
{
	return (((uint32_t)p[0] << 16 | (uint32_t)p[1] << 8 | p[2]) * 0x9E3779B1u) >> (32 - HASH_BITS);
}

static void deflate(out_t *o, const uint8_t *in, size_t size)
{
	int32_t		*head, *prev;
	uint32_t	*tokens;
	int			num_tokens = 0;
	size_t		pos = 0, i;

	head = (int32_t *)malloc(HASH_SIZE * sizeof(int32_t));
	prev = (int32_t *)malloc(WINDOW * sizeof(int32_t));
	tokens = (uint32_t *)calloc(BLOCK_TOKENS, sizeof(uint32_t));
	if (!head || !prev || !tokens)
	{
		o->failed = 1;
		free(head);
		free(prev);
		free(tokens);
		return;
	}
	for (i = 0; i < HASH_SIZE; i++)
		head[i] = -1;

	while (pos < size)
	{
		int best_len = 0, best_dist = 0;

		if (pos + 3 <= size)
		{
			const uint32_t h = hash3(in + pos);
			const size_t max_len = size - pos < 258 ? size - pos : 258;
			int32_t at = head[h];
			int chain = MAX_CHAIN;

			while (at >= 0 && pos - (size_t)at <= WINDOW && chain--)
			{
				const uint8_t *a = in + at, *b = in + pos;
				size_t len = 0;

				while (len < max_len && a[len] == b[len])
					len++;
				if ((int)len > best_len)
				{
					best_len = (int)len;
					best_dist = (int)(pos - (size_t)at);
					if (len == max_len)
						break;
				}
				at = prev[at & (WINDOW - 1)];
			}
		}

		if (best_len >= 3)
		{
			tokens[num_tokens++] = COPY(best_len, best_dist);
			for (i = 0; i < (size_t)best_len; i++, pos++)
			{
				if (pos + 3 <= size)
				{
					const uint32_t h = hash3(in + pos);
					prev[pos & (WINDOW - 1)] = head[h];
					head[h] = (int32_t)pos;
				}
			}
		}
		else
		{
			if (pos + 3 <= size)
			{
				const uint32_t h = hash3(in + pos);
				prev[pos & (WINDOW - 1)] = head[h];
				head[h] = (int32_t)pos;
			}
			tokens[num_tokens++] = in[pos++];
		}

		if (num_tokens == BLOCK_TOKENS || pos == size)
		{
			write_block(o, tokens, num_tokens, pos == size);
			num_tokens = 0;
		}
	}
	if (!size)
		write_block(o, tokens, 0, 1);
	flush_bits(o);

	free(head);
	free(prev);
	free(tokens);
}

/* ---------------------------------------------------------------------- PNG */

static uint32_t crc_table[256];

static uint32_t crc32(uint32_t crc, const uint8_t *data, size_t size)
{
	size_t i;

	if (!crc_table[1])
	{
		uint32_t n, k;
		for (n = 0; n < 256; n++)
		{
			uint32_t c = n;
			for (k = 0; k < 8; k++)
				c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
			crc_table[n] = c;
		}
	}
	crc = ~crc;
	for (i = 0; i < size; i++)
		crc = crc_table[(crc ^ data[i]) & 0xff] ^ (crc >> 8);
	return ~crc;
}

static void put_be32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)(v >> 24);
	p[1] = (uint8_t)(v >> 16);
	p[2] = (uint8_t)(v >> 8);
	p[3] = (uint8_t)v;
}

static int write_chunk(FILE *f, const char *type, const uint8_t *data, size_t size)
{
	uint8_t		head[8], tail[4];
	uint32_t	crc;

	put_be32(head, (uint32_t)size);
	memcpy(head + 4, type, 4);
	crc = crc32(0, head + 4, 4);
	if (size)
		crc = crc32(crc, data, size);
	put_be32(tail, crc);
	return fwrite(head, 1, 8, f) == 8 && (!size || fwrite(data, 1, size, f) == size) && fwrite(tail, 1, 4, f) == 4;
}

static int paeth(int a, int b, int c)
{
	const int p = a + b - c;
	const int pa = abs(p - a), pb = abs(p - b), pc = abs(p - c);
	return (pa <= pb && pa <= pc) ? a : (pb <= pc ? b : c);
}

/* one row as differences from its neighbours, by the given PNG filter */
static void filter_row(int type, const uint8_t *row, const uint8_t *above, int bytes, uint8_t *out)
{
	int i;

	for (i = 0; i < bytes; i++)
	{
		const int left = i >= 3 ? row[i - 3] : 0;
		const int up = above ? above[i] : 0;
		const int corner = (above && i >= 3) ? above[i - 3] : 0;
		int predicted;

		switch (type)
		{
		case 1: predicted = left; break;
		case 2: predicted = up; break;
		case 3: predicted = (left + up) >> 1; break;
		case 4: predicted = paeth(left, up, corner); break;
		default: predicted = 0; break;
		}
		out[i] = (uint8_t)(row[i] - predicted);
	}
}

int pt_png_write(const char *path, const uint32_t *pixels, int width, int height)
{
	static const uint8_t signature[8] = {0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a};
	const int	bytes = width * 3;
	uint8_t		*rgb, *raw, *trial;
	uint8_t		header[13];
	uint32_t	a = 1, b = 0;
	size_t		raw_size, i;
	out_t		o;
	FILE		*f;
	int			x, y, ok;

	if (width <= 0 || height <= 0)
		return 0;

	raw_size = (size_t)(bytes + 1) * height;
	rgb = (uint8_t *)malloc((size_t)bytes * height);
	raw = (uint8_t *)malloc(raw_size);
	trial = (uint8_t *)malloc(bytes);
	if (!rgb || !raw || !trial)
	{
		free(rgb);
		free(raw);
		free(trial);
		return 0;
	}

	for (y = 0; y < height; y++)
	{
		const uint32_t *in = pixels + (size_t)y * width;
		uint8_t *out = rgb + (size_t)y * bytes;
		for (x = 0; x < width; x++)
		{
			out[x * 3] = (uint8_t)in[x];
			out[x * 3 + 1] = (uint8_t)(in[x] >> 8);
			out[x * 3 + 2] = (uint8_t)(in[x] >> 16);
		}
	}

	/* each row with whichever filter leaves the smallest differences */
	for (y = 0; y < height; y++)
	{
		const uint8_t *row = rgb + (size_t)y * bytes;
		const uint8_t *above = y ? row - bytes : NULL;
		uint8_t *out = raw + (size_t)y * (bytes + 1);
		long best = -1;
		int type;

		for (type = 0; type < 5; type++)
		{
			long sum = 0;

			filter_row(type, row, above, bytes, trial);
			for (x = 0; x < bytes; x++)
				sum += abs((int8_t)trial[x]);
			if (best < 0 || sum < best)
			{
				best = sum;
				out[0] = (uint8_t)type;
				memcpy(out + 1, trial, bytes);
			}
		}
	}
	free(rgb);
	free(trial);

	memset(&o, 0, sizeof(o));
	put_byte(&o, 0x78);		/* zlib: deflate, 32K window */
	put_byte(&o, 0x9c);
	deflate(&o, raw, raw_size);
	for (i = 0; i < raw_size; i++)		/* Adler-32 of what was packed */
	{
		a += raw[i];
		b += a;
		if ((i & 4095) == 4095)
		{
			a %= 65521;
			b %= 65521;
		}
	}
	a %= 65521;
	b %= 65521;
	put_byte(&o, (uint8_t)(b >> 8));
	put_byte(&o, (uint8_t)b);
	put_byte(&o, (uint8_t)(a >> 8));
	put_byte(&o, (uint8_t)a);
	free(raw);

	if (o.failed)
	{
		free(o.data);
		return 0;
	}

	f = fopen(path, "wb");
	if (!f)
	{
		free(o.data);
		return 0;
	}
	put_be32(header, (uint32_t)width);
	put_be32(header + 4, (uint32_t)height);
	header[8] = 8;		/* bits per channel */
	header[9] = 2;		/* red, green, blue */
	header[10] = header[11] = header[12] = 0;

	ok = fwrite(signature, 1, 8, f) == 8
		&& write_chunk(f, "IHDR", header, 13)
		&& write_chunk(f, "IDAT", o.data, o.size)
		&& write_chunk(f, "IEND", NULL, 0);
	if (fclose(f))
		ok = 0;
	free(o.data);
	return ok;
}
