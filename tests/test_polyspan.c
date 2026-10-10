// test_polyspan.c -- D_PolygonSpans (d_polyspan.c), which sprites, fences,
// translucent faces and flat polygons are drawn from. Whatever the polygon (in
// the view, around its edges, a corner near the eye a pixel or two out, far off
// it, degenerate, not a number), its spans are lines of the view rectangle, one
// each and in order, inside it, no more than its height, and nothing is written
// past the room the drawers give them. A polygon inside the view covers the
// pixels its edges do.

#include "d_local.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#define MAX_POINTS	16
#define GUARD		16			// spans past the room the drawers give, not to be written
#define UNWRITTEN	(-77)
#define P(pu, pv)		{.u = (pu), .v = (pv)}
#define RECT(l, t, w, h)	{.x = (l), .y = (t), .width = (w), .height = (h)}

static int		failures, polygons;
static unsigned	seed = 1;

static float Random (void)
{
	seed ^= seed << 13;
	seed ^= seed >> 17;
	seed ^= seed << 5;
	return (float)(seed >> 8) / (float)(1 << 24);
}

static float RandomRange (float lo, float hi)
{
	return lo + (hi - lo) * Random ();
}

static void Fail (const char *what, const vrect_t *rect, const emitpoint_t *in, int nump, int span)
{
	int		i;

	if (++failures > 20)
		return;
	printf ("FAILED %s: span %i, rect %i %i %i %i, polygon", what, span, rect->x, rect->y, rect->width, rect->height);
	for (i = 0 ; i < nump ; i++)
		printf (" (%.9g %.9g)", in[i].u, in[i].v);
	printf ("\n");
}

// the polygon's left and right at line y, as the pixel centres it covers
// begin and end: ceil of where its edges cross it
static void Across (const emitpoint_t *p, int nump, double y, double *left, double *right)
{
	int		i, j;
	double	u0, v0, u1, v1, x;

	*left = 1e30;
	*right = -1e30;
	for (i = 0 ; i < nump ; i++)
	{
		j = i + 1 < nump ? i + 1 : 0;
		u0 = p[i].u; v0 = p[i].v; u1 = p[j].u; v1 = p[j].v;
		if (y < fmin (v0, v1) || y > fmax (v0, v1))
			continue;
		x = v0 == v1 ? u0 : u0 + (y - v0) * (u1 - u0) / (v1 - v0);
		*left = fmin (*left, v0 == v1 ? fmin (u0, u1) : x);
		*right = fmax (*right, v0 == v1 ? fmax (u0, u1) : x);
	}
}

static void Test (const emitpoint_t *in, int nump, const vrect_t *rect)
{
	static sspan_t	*spans;
	static int		room;
	emitpoint_t		p[MAX_POINTS + 1];
	sspan_t			*s;
	double			area, left, right;
	int				i, j, n, need, inside;
	float			x0, x1, y0, y1;

	polygons++;
	need = rect->height + 1 + GUARD;
	if (need > room)
	{
		room = need;
		spans = realloc (spans, (size_t)room * sizeof(*spans));
	}
	for (i = 0 ; i < need ; i++)
		spans[i].u = spans[i].v = spans[i].count = UNWRITTEN;

	// the winding a front face has on screen, as R_DrawFence turns it
	area = 0;
	for (i = 0 ; i < nump ; i++)
	{
		j = i + 1 < nump ? i + 1 : 0;
		area += (double)in[i].u * in[j].v - (double)in[j].u * in[i].v;
	}
	for (i = 0 ; i < nump ; i++)
		p[i] = area < 0 ? in[nump - 1 - i] : in[i];
	p[nump].u = p[nump].v = (float)UNWRITTEN;

	x0 = (float)rect->x - 0.5f;
	x1 = (float)(rect->x + rect->width) - 0.5f;
	y0 = (float)rect->y - 0.5f;
	y1 = (float)(rect->y + rect->height) - 0.5f;
	inside = 1;
	for (i = 0 ; i < nump ; i++)
		if (!(p[i].u >= x0 && p[i].u <= x1 && p[i].v >= y0 && p[i].v <= y1))
			inside = 0;

	if (!D_PolygonSpans (p, nump, spans, rect))
		n = 0;
	else
		for (n = 0 ; n < rect->height + 1 && spans[n].count != DS_SPAN_LIST_END ; n++)
			;
	if (n > rect->height || (n && spans[n].count != DS_SPAN_LIST_END))
	{
		Fail ("more lines than the view has", rect, in, nump, n);
		return;
	}
	for (i = rect->height + 1 ; i < need ; i++)
		if (spans[i].u != UNWRITTEN || spans[i].v != UNWRITTEN || spans[i].count != UNWRITTEN)
		{
			Fail ("written past the room for the view's lines", rect, in, nump, i);
			return;
		}

	for (i = 0, s = spans ; i < n ; i++, s++)
	{
		if (s->v < rect->y || s->v >= rect->y + rect->height || (i && s->v != s[-1].v + 1))
		{
			Fail ("a line outside the view, or out of order", rect, in, nump, i);
			return;
		}
		if (s->u < rect->x || s->u > rect->x + rect->width
			|| (s->count > 0 && s->u + s->count > rect->x + rect->width))
		{
			Fail ("a span outside the view", rect, in, nump, i);
			return;
		}
		if (!inside)
			continue;
		// a polygon in the view as it was: its edges' pixels, give or take
		// one where an edge passes through a pixel's centre
		Across (p, nump, s->v, &left, &right);
		if (fabs (s->u - ceil (left)) > 1 || fabs (s->u + s->count - ceil (right)) > 1)
		{
			Fail ("not the pixels its edges cover", rect, in, nump, i);
			return;
		}
	}
}

// a convex polygon: n corners of an ellipse, in order
static int Convex (emitpoint_t *p, float cx, float cy, float rx, float ry)
{
	float	angles[MAX_POINTS], a;
	int		n, i, j;

	n = 1 + (int)(Random () * 8);
	for (i = 0 ; i < n ; i++)
		angles[i] = RandomRange (0, 6.2831853f);
	for (i = 1 ; i < n ; i++)
		for (j = i ; j > 0 && angles[j - 1] > angles[j] ; j--)
		{
			a = angles[j];
			angles[j] = angles[j - 1];
			angles[j - 1] = a;
		}
	for (i = 0 ; i < n ; i++)
	{
		p[i].u = cx + rx * cosf (angles[i]);
		p[i].v = cy + ry * sinf (angles[i]);
		p[i].s = p[i].t = p[i].zi = 0;
	}
	return n;
}

static void Fixed (const vrect_t *rect)
{
	// a translucent face against the eye (AD): a corner clipped to the view's
	// planes, 0.08 units off, came out a pixel above and left of the view
	static const emitpoint_t	eye[] = {
		P(135.770523f, -0.500336f), P(-0.501892f, 88.894867f), P(-1.091202f, -1.068192f)};
	float	w = (float)rect->width, h = (float)rect->height, x = (float)rect->x, y = (float)rect->y;
	float	inf = INFINITY, nan = NAN;
	emitpoint_t	all[] = {P(x - 1e6f, y - 1e6f), P(x + 1e6f, y - 1e6f), P(x + 1e6f, y + 1e6f), P(x - 1e6f, y + 1e6f)};
	emitpoint_t	big[] = {P(-1e30f, -1e30f), P(1e30f, 0), P(0, 1e30f)};
	emitpoint_t	notnum[] = {P(nan, nan), P(x + w / 2, y + h / 2), P(x + 1, y + h - 1)};
	emitpoint_t	infinite[] = {P(-inf, -inf), P(inf, y + h / 2), P(x + w / 2, inf)};
	emitpoint_t	flat[] = {P(x - 5, y + h / 2), P(x + w + 5, y + h / 2), P(x + w / 2, y + h / 2)};
	emitpoint_t	line[] = {P(x - 3, y - 3), P(x + w + 3, y + h + 3)};
	emitpoint_t	point[] = {P(x + w / 2, y + h / 2)};
	emitpoint_t	thin[] = {P(x + 0.1f, y - 2), P(x + 0.2f, y + h + 2), P(x + 0.15f, y + h + 2)};
	emitpoint_t	under[] = {P(x + w / 2, y + h - 0.49f), P(x + w + 1, y + h + 0.7f), P(x - 1, y + h + 0.6f)};
	sspan_t		*s;
	int			i;

	Test (eye, 3, rect);
	Test (big, 3, rect);
	Test (notnum, 3, rect);
	Test (infinite, 3, rect);
	Test (flat, 3, rect);
	Test (line, 2, rect);
	Test (point, 1, rect);
	Test (thin, 3, rect);
	Test (under, 3, rect);

	// past the view on every side: the whole of it
	{
		emitpoint_t	p[5];
		sspan_t		*spans = calloc ((size_t)rect->height + 1, sizeof(*spans));

		for (i = 0 ; i < 4 ; i++)
			p[i] = all[i];
		polygons++;
		if (!D_PolygonSpans (p, 4, spans, rect))
			Fail ("covering the view, no lines", rect, all, 4, 0);
		else
		{
			for (i = 0, s = spans ; i < rect->height ; i++, s++)
				if (s->v != rect->y + i || s->u != rect->x || s->count != rect->width)
				{
					Fail ("covering the view, not all of a line", rect, all, 4, i);
					break;
				}
			if (s->count != DS_SPAN_LIST_END)
				Fail ("covering the view, not ended", rect, all, 4, i);
		}
		free (spans);
	}
}

int main (void)
{
	static const vrect_t	rects[] = {
		RECT(0, 0, 320, 200), RECT(0, 0, 1, 1), RECT(0, 0, 2, 640), RECT(0, 0, 1920, 1080), RECT(3, 5, 17, 9), RECT(40, 24, 240, 152)};
	emitpoint_t	p[MAX_POINTS];
	const vrect_t	*rect;
	float		x, y, w, h, cx, cy, r, edge;
	int			k, i, j, n;

	setvbuf (stdout, NULL, _IONBF, 0);	// what failed, before a crash
	for (k = 0 ; k < (int)(sizeof(rects) / sizeof(rects[0])) ; k++)
	{
		rect = &rects[k];
		x = (float)rect->x;
		y = (float)rect->y;
		w = (float)rect->width;
		h = (float)rect->height;
		Fixed (rect);
		for (i = 0 ; i < 40000 ; i++)
		{
			switch (i % 4)
			{
			case 0:		// anywhere about the view, any size
				cx = RandomRange (x - 2 * w, x + 3 * w);
				cy = RandomRange (y - 2 * h, y + 3 * h);
				r = expf (RandomRange (-5, logf (10 * (w + h))));
				n = Convex (p, cx, cy, r * RandomRange (0.05f, 1), r * RandomRange (0.05f, 1));
				break;
			case 1:		// inside the view
				r = RandomRange (0.01f, 0.5f) * fminf (w, h);
				cx = RandomRange (x - 0.5f + r, x + w - 0.5f - r);
				cy = RandomRange (y - 0.5f + r, y + h - 0.5f - r);
				n = Convex (p, cx, cy, r * RandomRange (0.05f, 1), r * RandomRange (0.05f, 1));
				break;
			default:	// as the view's planes leave one: a corner or edge a few pixels out
				r = RandomRange (1, w + h);
				cx = RandomRange (x, x + w);
				cy = RandomRange (y, y + h);
				n = Convex (p, cx, cy, r, r * RandomRange (0.1f, 1));
				edge = RandomRange (-0.5f - 3, -0.5f);
				for (j = 0 ; j < n ; j++)
				{
					if (p[j].u < x - 0.5f) p[j].u = x + edge;
					if (p[j].u > x + w - 0.5f) p[j].u = x + w - 1 - edge;
					if (p[j].v < y - 0.5f) p[j].v = y + edge;
					if (p[j].v > y + h - 0.5f) p[j].v = y + h - 1 - edge;
				}
				break;
			}
			Test (p, n, rect);
		}
	}

	printf ("%i polygons, %i failed\n", polygons, failures);
	return failures ? 1 : 0;
}
