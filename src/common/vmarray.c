// vmarray.c -- contiguous arrays that grow in place

#include "sys.h"
#include "vmarray.h"
#include "sys.h"

#include <stdint.h>
#include <string.h>

void VMArray_Init (vmarray_t *array, const char *name, size_t elemsize, size_t maxcount)
{
	memset (array, 0, sizeof (*array));
	if (!elemsize || maxcount > SIZE_MAX / elemsize)
		Sys_Error ("VMArray_Init: bad size for %s", name);
	array->name = name;
	array->elemsize = elemsize;
	array->maxcount = maxcount;
	array->base = Sys_ReserveMemory (elemsize * maxcount);
}

void VMArray_Resize (vmarray_t *array, size_t count)
{
	if (count <= array->count)
		return;
	if (count > array->maxcount)
		Sys_Error ("%s: limit of %zu exceeded", array->name, array->maxcount);

	Sys_CommitMemory (array->base, count * array->elemsize);
	array->count = count;
}

void *VMArray_Reserve (vmarray_t *array, size_t used, size_t n)
{
	if (n > SIZE_MAX - used)
		Sys_Error ("%s: size overflow", array->name);
	VMArray_Resize (array, used + n);
	return array->base + used * array->elemsize;
}

void VMArray_Clear (vmarray_t *array)
{
	if (array->count)
		memset (array->base, 0, array->count * array->elemsize);
}

void VMArray_Free (vmarray_t *array)
{
	if (array->base)
		Sys_ReleaseMemory (array->base, array->maxcount * array->elemsize);
	memset (array, 0, sizeof (*array));
}
