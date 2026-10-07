// test_qc_fixtures.c -- the QuakeC fixtures (qc/), compiled with fteqcc for the
// targets each is meant for, run with the builtins of FTE's standalone runner:
// what they print must be their .expected file, and what FTE's runner prints
// when FTE_QCVM names it

#include "qc_runner.h"
#include "qc_test.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define popen	_popen
#define pclose	_pclose
#endif

// the text with CRLFs as LFs (malloc'd)
static char *Unix (const char *text)
{
	char	*out = malloc (strlen (text) + 1), *o = out;

	for ( ; *text ; text++)
		if (!(text[0] == '\r' && text[1] == '\n'))
			*o++ = *text;
	*o = 0;
	return out;
}

// what the fixture prints under this VM, and "ERROR: ..." if it fails
static char *RunOurs (const uint8_t *dat, size_t size)
{
	qc_progs_t		*p;
	qc_builtins_t	*b = QR_Builtins ();
	qc_config_t		config;
	qc_runnerhost_t	host = {{0}};
	qcvm_t			*vm;
	char			text[1024];
	char			*out;

	QT_TextAppend (&host.out, "");
	p = QC_LoadProgs (dat, size, NULL);
	if (!p)
	{
		QC_BuiltinsFree (b);
		return Unix ("ERROR: the fixture doesn't load\n");
	}
	QC_DefaultConfig (&config, QC_CSQC);
	config.limits.local_stack_words = 1u << 16;
	vm = QC_Create (p, b, &config, &(qc_host_t){0}, &host, NULL);
	QC_ReleaseProgs (p);
	if (!QC_Call (vm, QC_FindFunction (vm, "main"), 0, NULL, NULL))
	{
		QT_TextAppend (&host.out, "ERROR: ");
		QT_TextAppend (&host.out, QC_ErrorText (QC_LastError (vm), text, sizeof(text)));
		QT_TextAppend (&host.out, "\n");
	}
	QC_Destroy (vm);
	QC_BuiltinsFree (b);
	out = Unix (host.out.text);
	QT_TextFree (&host.out);
	return out;
}

// what FTE's own runner prints, if FTE_QCVM names it
static char *RunOracle (const char *path)
{
	const char	*runner = getenv ("FTE_QCVM");
	char		command[2048], buf[4096];
	qt_text_t	out = {0};
	FILE		*f;
	size_t		n;
	char		*text;

	if (!runner || !*runner)
		return NULL;
	snprintf (command, sizeof(command), "\"%s\" \"%s\"", runner, path);
	f = popen (command, "r");
	if (!f)
		return NULL;
	QT_TextAppend (&out, "");
	while ((n = fread (buf, 1, sizeof(buf) - 1, f)) > 0)
	{
		buf[n] = 0;
		QT_TextAppend (&out, buf);
	}
	pclose (f);
	text = Unix (out.text);
	QT_TextFree (&out);
	return text;
}

static void CheckFixture (const char *base, const char *label)
{
	char	path[1024], expectedpath[1024];
	uint8_t	*dat, *expected;
	size_t	size, esize;
	char	*ours, *oracle, *want;

	snprintf (path, sizeof(path), "%s/%s-%s.dat", QT_FIXTURE_DIR, base, label);
	snprintf (expectedpath, sizeof(expectedpath), "%s/%s.qc.expected", QT_FIXTURE_SOURCES, base);
	dat = QT_LoadFile (path, &size);
	if (!QT_CHECK (dat != NULL))
	{
		printf ("  no %s\n", path);
		return;
	}
	ours = RunOurs (dat, size);
	free (dat);
	expected = QT_LoadFile (expectedpath, &esize);
	want = expected ? Unix ((char *)expected) : NULL;
	free (expected);
	oracle = RunOracle (path);
	if (oracle)
	{
		if (!QT_EQ_S (ours, oracle))
			printf ("  %s [%s]: ours against FTE's runner\n", base, label);
		if (want && !QT_EQ_S (oracle, want))
			printf ("  %s: FTE's output drifted from .expected\n", base);
	}
	else if (QT_CHECK (want != NULL))
	{
		if (!QT_EQ_S (ours, want))
			printf ("  %s [%s]\n", base, label);
	}
	free (ours);
	free (oracle);
	free (want);
}

int main (void)
{
	char	list[] = QT_FIXTURES, *entry, *label, *next;
	int		count = 0;

	for (entry = list ; entry ; entry = next)
	{
		next = strchr (entry, ',');
		if (next)
			*next++ = 0;
		label = strchr (entry, '|');
		if (!label)
			continue;
		*label++ = 0;
		CheckFixture (entry, label);
		count++;
	}
	printf ("fixtures: %d runs\n", count);
	return QT_Finish ("fixtures", "every fixture prints what FTE's runner prints");
}
