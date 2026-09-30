// qc_heap.c -- the QuakeC heap (memalloc and friends): first fit over one region
//
// The block lists live outside the region, so QuakeC's writes can't corrupt
// them. Blocks are 16-byte granular and come zeroed.

#include "qc_local.h"

#include <stdlib.h>

#define HEAP_ALIGN	16u

bool QC_HeapInit (qc_heap_t *h, uint32_t max)
{
	*h = (qc_heap_t){.max = max & ~(HEAP_ALIGN - 1)};
	return QC_RegionReserve (&h->region, h->max);
}

void QC_HeapFree (qc_heap_t *h)
{
	QC_RegionFree (&h->region);
	free (h->used);
	free (h->free);
	*h = (qc_heap_t){0};
}

// the index of the first block at or after ofs
static uint32_t QC_BlockSearch (const qc_heapblock_t *list, uint32_t count, uint32_t ofs)
{
	uint32_t	lo = 0, hi = count, mid;

	while (lo < hi)
	{
		mid = lo + (hi - lo) / 2;
		if (list[mid].ofs < ofs)
			lo = mid + 1;
		else
			hi = mid;
	}
	return lo;
}

static bool QC_BlockInsert (qc_heapblock_t **list, uint32_t *count, uint32_t *size, uint32_t at, qc_heapblock_t b)
{
	qc_heapblock_t	*grown;
	uint32_t		n;

	if (*count == *size)
	{
		n = *size ? *size * 2 : 16;
		grown = realloc (*list, (size_t)n * sizeof(*grown));
		if (!grown)
			return false;
		*list = grown;
		*size = n;
	}
	memmove (*list + at + 1, *list + at, (size_t)(*count - at) * sizeof(**list));
	(*list)[at] = b;
	(*count)++;
	return true;
}

static void QC_BlockDelete (qc_heapblock_t *list, uint32_t *count, uint32_t at)
{
	memmove (list + at, list + at + 1, (size_t)(*count - at - 1) * sizeof(*list));
	(*count)--;
}

bool QC_HeapAlloc (qc_heap_t *h, uint32_t n, uint32_t *ofs)
{
	uint32_t		size, i, offset, end;
	qc_heapblock_t	rest;

	size = n ? n : 1;
	if (size > UINT32_MAX - (HEAP_ALIGN - 1))
		return false;
	size = (size + HEAP_ALIGN - 1) & ~(HEAP_ALIGN - 1);

	for (i = 0 ; i < h->numfree ; i++)
		if (h->free[i].size >= size)
			break;
	if (i < h->numfree)
	{
		offset = h->free[i].ofs;
		if (h->free[i].size > size)
		{
			rest = (qc_heapblock_t){offset + size, h->free[i].size - size, 0};
			h->free[i] = rest;
		}
		else
			QC_BlockDelete (h->free, &h->numfree, i);
	}
	else
	{
		offset = h->len;
		if (size > h->max - offset)
			return false;
		end = offset + size;
		if (!QC_RegionCommit (&h->region, end))
			return false;
		h->len = end;
	}
	memset (h->region.base + offset, 0, size);
	i = QC_BlockSearch (h->used, h->numused, offset);
	if (!QC_BlockInsert (&h->used, &h->numused, &h->usedsize, i, (qc_heapblock_t){offset, size, n}))
	{
		// keep the space: give it back to the free list
		QC_BlockInsert (&h->free, &h->numfree, &h->freesize, QC_BlockSearch (h->free, h->numfree, offset),
			(qc_heapblock_t){offset, size, 0});
		return false;
	}
	*ofs = offset;
	return true;
}

bool QC_HeapRelease (qc_heap_t *h, uint32_t ofs)
{
	uint32_t	i = QC_BlockSearch (h->used, h->numused, ofs);
	uint32_t	start, len, at;

	if (i == h->numused || h->used[i].ofs != ofs)
		return false;
	start = ofs;
	len = h->used[i].size;
	QC_BlockDelete (h->used, &h->numused, i);

	at = QC_BlockSearch (h->free, h->numfree, start);
	// merge with the free block after it
	if (at < h->numfree && h->free[at].ofs == start + len)
	{
		len += h->free[at].size;
		QC_BlockDelete (h->free, &h->numfree, at);
	}
	// and the one before it
	if (at > 0 && h->free[at - 1].ofs + h->free[at - 1].size == start)
	{
		h->free[at - 1].size += len;
		return true;
	}
	if (!QC_BlockInsert (&h->free, &h->numfree, &h->freesize, at, (qc_heapblock_t){start, len, 0}))
		return true;		// the space is lost, but the block is freed
	return true;
}

bool QC_HeapBlockSize (const qc_heap_t *h, uint32_t ofs, uint32_t *size)
{
	uint32_t	i = QC_BlockSearch (h->used, h->numused, ofs);

	if (i == h->numused || h->used[i].ofs != ofs)
		return false;
	*size = h->used[i].requested;
	return true;
}

bool QC_HeapRealloc (qc_heap_t *h, bool had, uint32_t old, uint32_t n, uint32_t *ofs)
{
	uint32_t	oldsize, keep;

	if (!had)
		return QC_HeapAlloc (h, n, ofs);
	if (!QC_HeapBlockSize (h, old, &oldsize) || !QC_HeapAlloc (h, n, ofs))
		return false;
	keep = oldsize < n ? oldsize : n;
	memmove (h->region.base + *ofs, h->region.base + old, keep);
	QC_HeapRelease (h, old);
	return true;
}
