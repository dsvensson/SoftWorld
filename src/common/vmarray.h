// vmarray.h -- contiguous arrays that grow in place.
//
// A vmarray reserves address space for its maximum size up front and commits
// memory as it grows, so its base address never changes and pointers into it stay
// valid. Committed memory starts out zeroed. Used where data must be contiguous
// and addressed by offset, like QuakeC edicts and the QuakeC string pool.
#pragma once

#include <stddef.h>

typedef struct vmarray_s
{
	unsigned char	*base;
	size_t			elemsize;
	size_t			maxcount;		// elements reserved
	size_t			count;			// elements committed
	const char		*name;
} vmarray_t;

void	VMArray_Init (vmarray_t *array, const char *name, size_t elemsize, size_t maxcount);

// makes sure at least count elements are committed; fatal beyond maxcount
void	VMArray_Resize (vmarray_t *array, size_t count);

// commits room for n more elements after `used` and returns a pointer to the first
void	*VMArray_Reserve (vmarray_t *array, size_t used, size_t n);
