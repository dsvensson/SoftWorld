// qc_mem.c -- VM memory: its regions, where an address lands, and entity slots

#include "qc_local.h"
#include "sys.h"

#include <stdlib.h>

// commits grow by at least this, to keep the system calls few
#define COMMIT_STEP		(64u << 10)

bool QC_RegionReserve (qc_region_t *r, size_t size)
{
	*r = (qc_region_t){0};
	if (!size)
		return true;
	size = (size + COMMIT_STEP - 1) & ~(size_t)(COMMIT_STEP - 1);
	r->base = Sys_TryReserveMemory (size);
	if (!r->base)
		return false;
	r->reserved = size;
	return true;
}

bool QC_RegionCommit (qc_region_t *r, size_t size)
{
	size_t	want;

	if (size <= r->committed)
		return true;
	if (size > r->reserved)
		return false;
	want = (size + COMMIT_STEP - 1) & ~(size_t)(COMMIT_STEP - 1);
	if (want > r->reserved)
		want = r->reserved;
	if (!Sys_TryCommitMemory (r->base, want))
		return false;
	r->committed = want;
	return true;
}

void QC_RegionFree (qc_region_t *r)
{
	Sys_ReleaseMemory (r->base, r->reserved);
	*r = (qc_region_t){0};
}

/*
==============================================================================

ADDRESSES

==============================================================================
*/

qc_loc_t QC_Locate (const qc_mem_t *m, uint32_t p, uint32_t n)
{
	uint64_t	end = (uint64_t)p + n;
	uint32_t	off, e, within;

	if (end <= m->s_len)
		return (qc_loc_t){QC_LOC_S, m->s.base + p, 0, m->s_len - p};
	if (p >= m->e_base)
	{
		off = p - m->e_base;
		e = off >> m->shift;
		within = off & ((1u << m->shift) - 1);
		if (e < m->num_edicts && (uint64_t)within + n <= m->field_bytes)
			return (qc_loc_t){QC_LOC_E, m->fields + off, e, m->field_bytes - within};
	}
	if (p >= m->h_base)
	{
		off = p - m->h_base;
		if ((uint64_t)off + n <= m->heap.len)
			return (qc_loc_t){QC_LOC_H, m->heap.region.base + off, 0, m->heap.len - off};
	}
	return (qc_loc_t){QC_LOC_NONE, NULL, 0, 0};
}

bool QC_ReadBytes (const qc_mem_t *m, uint32_t p, void *out, uint32_t n)
{
	qc_loc_t	loc = QC_Locate (m, p, n);

	if (loc.type == QC_LOC_NONE)
		return false;
	memcpy (out, loc.p, n);
	return true;
}

qc_writeresult_t QC_CheckWrite (const qc_mem_t *m, uint32_t p, uint32_t n, qc_loc_t *loc)
{
	if (!p)
		return QC_WRITE_NULL;
	*loc = QC_Locate (m, p, n);
	if (loc->type == QC_LOC_NONE)
		return QC_WRITE_INVALID;
	if (loc->type == QC_LOC_E && m->slots[loc->ent].protected)
		return QC_WRITE_PROTECTED;
	return QC_WRITE_OK;
}

qc_writeresult_t QC_WriteBytes (qc_mem_t *m, uint32_t p, const void *bytes, uint32_t n, uint32_t *ent)
{
	qc_loc_t			loc;
	qc_writeresult_t	r = QC_CheckWrite (m, p, n, &loc);

	if (r == QC_WRITE_OK)
		memmove (loc.p, bytes, n);
	else if (r == QC_WRITE_PROTECTED && ent)
		*ent = loc.ent;
	return r;
}

uint32_t QC_GetS (const qc_mem_t *m, uint64_t ofs)
{
	uint32_t	v;

	if (ofs + 4 > m->s_len)
		return 0;
	memcpy (&v, m->s.base + ofs, 4);
	return v;
}

void QC_SetS (qc_mem_t *m, uint64_t ofs, uint32_t v)
{
	if (ofs + 4 <= m->s_len)
		memcpy (m->s.base + ofs, &v, 4);
}

uint8_t *QC_FieldPtr (const qc_mem_t *m, uint32_t e, uint32_t word, uint32_t words)
{
	uint64_t	end = ((uint64_t)word + words) * 4;

	if (e >= m->num_edicts || end > m->field_bytes)
		return NULL;
	return m->fields + ((size_t)e << m->shift) + (size_t)word * 4;
}

const char *QC_LinearString (const qc_mem_t *m, uint32_t p)
{
	qc_loc_t	loc = QC_Locate (m, p, 1);

	if (loc.type == QC_LOC_NONE || !memchr (loc.p, 0, loc.avail))
		return NULL;
	return (const char *)loc.p;
}

/*
==============================================================================

ENTITY SLOTS

==============================================================================
*/

bool QC_GrowEdicts (qc_mem_t *m, uint32_t e)
{
	uint32_t		count = e + 1;
	qc_entslot_t	*grown;
	uint32_t		size;

	if (e >= m->max_edicts)
		return false;
	if (count <= m->num_edicts)
		return true;
	if (!QC_RegionCommit (&m->e, (size_t)count << m->shift))
		return false;
	if (count > m->slotsize)
	{
		size = m->slotsize ? m->slotsize : 64;
		while (size < count)
			size = size > m->max_edicts / 2 ? m->max_edicts : size * 2;
		grown = realloc (m->slots, (size_t)size * sizeof(*grown));
		if (!grown)
			return false;
		m->slots = grown;
		m->slotsize = size;
	}
	memset (m->slots + m->num_edicts, 0, (size_t)(count - m->num_edicts) * sizeof(*m->slots));
	m->num_edicts = count;
	return true;
}

// the fields and what is reserved for more; the host's header is left alone
void QC_ClearEntity (qc_mem_t *m, uint32_t e)
{
	memset (m->fields + ((size_t)e << m->shift), 0, ((size_t)1 << m->shift) - m->header);
}
