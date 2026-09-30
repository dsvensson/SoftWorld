// qc_runner.h -- the builtins of FTE's standalone qcvm test runner, so QuakeC
// fixtures print the same under both VMs
#pragma once

#include "qc_test.h"
#include "qcvm.h"

// what the host of a runner VM holds: everything the fixture printed
typedef struct
{
	qt_text_t	out;
} qc_runnerhost_t;

// the runner's builtins (numbered 1-23)
qc_builtins_t	*QR_Builtins (void);

// C's %g and %f, with nan and inf spelled as glibc spells them
void	QR_FormatG (double v, char *buf, size_t size);
void	QR_FormatF (double v, char *buf, size_t size);
