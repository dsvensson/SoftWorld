// test_qc_lib_file.c -- QuakeC's files (FTE's FRIK_FILE) on the harness's
// in-memory disk: the sandbox, reading lines and bytes, writing, the mmap
// modes, positions and sizes, names, and the handles' limits

#include "qc_harness.h"
#include "qc_lib.h"		// the library's state

#include <stdio.h>
#include <string.h>

static const char	*named[] = {"fseek64", "fsize64", NULL};

static qh_t *HarnessWith (const qc_config_t *config)
{
	return QH_New (QC_NUMBERING_CSQC, config, QH_Named, (void *)named);
}

static qh_t *Harness (void)
{
	return HarnessWith (NULL);
}

static void AddText (qh_t *h, const char *path, const char *text)
{
	QH_AddFile (h, path, text, strlen (text));
}

static float Open (qh_t *h, const char *name, float mode)
{
	return QH_Float (h, "fopen", ARGS (QH_S (h, name), F (mode)));
}

// a file's bytes as text ("" for none)
static const char *FileText (const qh_t *h, const char *path)
{
	static char		text[256];
	const uint8_t	*data;
	size_t			size;

	if (!(data = QH_File (h, path, &size)) || size >= sizeof(text))
		return "";
	memcpy (text, data, size);
	text[size] = 0;
	return text;
}

static uint64_t Wide (qc_value_t r)
{
	return r.w[0] | (uint64_t)r.w[1] << 32;
}

// fgets: a line at a time without its \n and \rs, an empty line empty, null at
// the end; the host's file closed with the handle
static void TestReadLines (void)
{
	qc_config_t	config;
	qh_t		*h;
	char		line[5000], rest[1000];
	const char	*text;
	float		f;

	QC_DefaultConfig (&config, QC_CSQC);
	config.developer = true;
	h = HarnessWith (&config);

	AddText (h, "data/notes.txt", "one\r\ntwo\n\nlast");
	f = Open (h, "notes.txt", 0);
	QT_EQ_F (f, 1000);
	QT_EQ_S (QH_String (h, "fgets", ARGS (F (f))), "one");
	QT_EQ_S (QH_String (h, "fgets", ARGS (F (f))), "two");
	QT_EQ_S (QH_String (h, "fgets", ARGS (F (f))), "");
	QT_EQ_S (QH_String (h, "fgets", ARGS (F (f))), "last");
	QT_CHECK (QH_OptString (h, "fgets", ARGS (F (f))) == NULL);
	QT_EQ_I (h->host.openfiles, 1);
	QH_Call (h, "fclose", ARGS (F (f)), NULL);
	QT_EQ_I (h->host.openfiles, 0);
	// FTE's developer print of each open (developer mode only)
	QT_CHECK (h->host.dprinted.text && strstr (h->host.dprinted.text, "qcfopen(\"notes.txt\", 0) called") != NULL);

	// a NUL is C0 80; a long line comes in pieces of 4095 bytes, nothing lost
	memset (line, 'x', sizeof(line));
	line[4094] = 'y';
	line[4095] = 'z';
	QH_AddFile (h, "data/long", line, sizeof(line));
	QH_AddFile (h, "data/nul", "a\0b", 3);
	f = Open (h, "long", 0);
	text = QH_String (h, "fgets", ARGS (F (f)));
	QT_CHECK (strlen (text) == 4095 && text[4094] == 'y');
	memset (rest, 'x', sizeof(rest));
	rest[0] = 'z';
	rest[sizeof(line) - 4095] = 0;
	QT_EQ_S (QH_String (h, "fgets", ARGS (F (f))), rest);
	QH_Call (h, "fclose", ARGS (F (f)), NULL);
	f = Open (h, "nul", 0);
	QT_EQ_S (QH_String (h, "fgets", ARGS (F (f))), "a\xC0\x80" "b");
	QH_Call (h, "fclose", ARGS (F (f)), NULL);
	QH_Free (h);
}

// FTE's sandbox: data/ first, then the name as it is (never for configs at the
// top or in configs/); names with "..", ':', '\' or a leading '/' refused
static void TestSandbox (void)
{
	static const char	*refused[] = {"../up.txt", "/abs.txt", "c:dos.txt", "back\\slash.txt", "a/../b", ""};
	qh_t				*h = Harness ();
	float				f;
	size_t				i;

	AddText (h, "plain.txt", "plain");
	AddText (h, "autoexec.cfg", "secret");
	AddText (h, "configs/game.cfg", "secret");
	AddText (h, "particles/fire.cfg", "fire");
	AddText (h, "data/mine.cfg", "mine");
	f = Open (h, "plain.txt", 0);
	QT_EQ_S (QH_String (h, "fgets", ARGS (F (f))), "plain");
	QH_Call (h, "fclose", ARGS (F (f)), NULL);
	QT_EQ_F (Open (h, "autoexec.cfg", 0), -1);
	QT_EQ_F (Open (h, "configs/game.cfg", 0), -1);
	f = Open (h, "particles/fire.cfg", 0);
	QT_EQ_S (QH_String (h, "fgets", ARGS (F (f))), "fire");
	QH_Call (h, "fclose", ARGS (F (f)), NULL);
	f = Open (h, "mine.cfg", 0);
	QT_EQ_S (QH_String (h, "fgets", ARGS (F (f))), "mine");
	QH_Call (h, "fclose", ARGS (F (f)), NULL);
	f = Open (h, "data/mine.cfg", 0);
	QT_EQ_S (QH_String (h, "fgets", ARGS (F (f))), "mine");
	QH_Call (h, "fclose", ARGS (F (f)), NULL);
	QT_EQ_F (Open (h, "missing.txt", 0), -1);

	QH_ClearWarnings (h);
	for (i = 0 ; i < sizeof(refused) / sizeof(refused[0]) ; i++)
		if (!QT_EQ_F (Open (h, refused[i], 2), -1))
			printf ("  \"%s\"\n", refused[i]);
	QT_EQ_I (QH_NumWarnings (h), (int)(sizeof(refused) / sizeof(refused[0])));
	// network streams (-1) and modes past mmap aren't files
	QT_EQ_F (Open (h, "plain.txt", -1), -1);
	QT_EQ_F (Open (h, "plain.txt", 7), -1);
	QT_EQ_I (h->host.openfiles, 0);
	QH_Free (h);
}

// writing: kept until fclose, then the whole file under data/; append after
// what data/'s file has
static void TestWrite (void)
{
	qh_t	*h = Harness ();
	float	f;

	f = Open (h, "out.txt", 2);
	QH_Call (h, "fputs", ARGS (F (f), QH_S (h, "abc"), QH_S (h, "def\n")), NULL);
	QT_CHECK (QH_File (h, "data/out.txt", &(size_t){0}) == NULL);
	QH_Call (h, "fclose", ARGS (F (f)), NULL);
	QT_EQ_S (FileText (h, "data/out.txt"), "abcdef\n");

	f = Open (h, "out.txt", 1);
	QH_Call (h, "fputs", ARGS (F (f), QH_S (h, "more")), NULL);
	QH_Call (h, "fclose", ARGS (F (f)), NULL);
	QT_EQ_S (FileText (h, "data/out.txt"), "abcdef\nmore");

	// append to a new file; write over an old one
	f = Open (h, "new.txt", 1);
	QH_Call (h, "fputs", ARGS (F (f), QH_S (h, "fresh")), NULL);
	QH_Call (h, "fclose", ARGS (F (f)), NULL);
	QT_EQ_S (FileText (h, "data/new.txt"), "fresh");
	f = Open (h, "out.txt", 2);
	QH_Call (h, "fclose", ARGS (F (f)), NULL);
	QT_EQ_S (FileText (h, "data/out.txt"), "");

	// a file to read takes no writing, and one to write gives no lines
	AddText (h, "data/ro.txt", "ro");
	f = Open (h, "ro.txt", 0);
	QH_Call (h, "fputs", ARGS (F (f), QH_S (h, "x")), NULL);
	QH_Call (h, "fclose", ARGS (F (f)), NULL);
	QT_EQ_S (FileText (h, "data/ro.txt"), "ro");
	f = Open (h, "w.txt", 2);
	QT_CHECK (QH_OptString (h, "fgets", ARGS (F (f))) == NULL);
	QH_Call (h, "fclose", ARGS (F (f)), NULL);
	QH_Free (h);
}

// bytes from and to VM memory, positions and sizes, 32- and 64-bit
static void TestBytes (void)
{
	static const uint8_t	bytes[10] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9};
	qh_t					*h = Harness ();
	uint32_t				p = QH_Word (h, "memalloc", ARGS (I (64)));
	uint8_t					got[16];
	const uint8_t			*data;
	size_t					size;
	float					f;

	QH_AddFile (h, "data/bin", bytes, sizeof(bytes));
	f = Open (h, "bin", 0);
	QT_EQ_U (Wide (QH_Raw (h, "fsize64", ARGS (F (f)))), 10);
	QT_EQ_I (QH_Int (h, "fsize", ARGS (F (f))), 10);
	QT_EQ_U (Wide (QH_Raw (h, "fseek64", ARGS (F (f), W (4)))), 0);
	QT_EQ_I (QH_Int (h, "fread", ARGS (F (f), W (p), I (64))), 6);
	QT_CHECK (QC_ReadMemory (h->vm, p, got, 6) && !memcmp (got, bytes + 4, 6));
	QT_EQ_I (QH_Int (h, "fread", ARGS (F (f), W (p), I (64))), 0);
	// the whole size must be memory, however little the file has left
	QT_EQ_I (QH_Fails (h, "fread", ARGS (F (f), W (p), I (65))), QC_ERR_BUILTIN);
	QT_EQ_I (QH_Int (h, "fseek", ARGS (F (f))), 10);
	// a negative position leaves it; the destination offset counts
	QT_EQ_I (QH_Int (h, "fseek", ARGS (F (f), I (-1))), 10);
	QT_EQ_I (QH_Int (h, "fseek", ARGS (F (f), I (1))), 10);
	QT_EQ_I (QH_Int (h, "fread", ARGS (F (f), W (p), I (2), I (8))), 2);
	QT_CHECK (QC_ReadMemory (h->vm, p + 8, got, 2) && got[0] == 1 && got[1] == 2);
	// sizes of a file to read don't change
	QH_ClearWarnings (h);
	QT_EQ_I (QH_Int (h, "fsize", ARGS (F (f), I (3))), 10);
	QT_EQ_I (QH_NumWarnings (h), 1);
	QT_EQ_I (QH_Int (h, "fsize", ARGS (F (f))), 10);
	QH_Call (h, "fclose", ARGS (F (f)), NULL);

	// fwrite from memory (a source offset), seeking back over it, truncating
	QC_WriteMemory (h->vm, p, bytes, sizeof(bytes));
	f = Open (h, "copy", 2);
	QT_EQ_I (QH_Int (h, "fwrite", ARGS (F (f), W (p), I (10))), 10);
	QT_EQ_I (QH_Int (h, "fseek", ARGS (F (f), I (2))), 10);
	QT_EQ_I (QH_Int (h, "fwrite", ARGS (F (f), W (p), I (1), I (9))), 1);
	QT_EQ_I (QH_Int (h, "fsize", ARGS (F (f), I (6))), 10);
	QT_EQ_I (QH_Int (h, "fsize", ARGS (F (f))), 6);
	QH_Call (h, "fclose", ARGS (F (f)), NULL);
	data = QH_File (h, "data/copy", &size);
	QT_CHECK (data && size == 6 && data[2] == 9 && data[3] == 3 && data[5] == 5);

	// growing a file past its end fills it with zeros, after a truncation too
	f = Open (h, "grow", 2);
	QH_Call (h, "fputs", ARGS (F (f), QH_S (h, "abcdef")), NULL);
	QH_Call (h, "fsize", ARGS (F (f), I (2)), NULL);
	QH_Call (h, "fsize", ARGS (F (f), I (4)), NULL);
	QH_Call (h, "fclose", ARGS (F (f)), NULL);
	data = QH_File (h, "data/grow", &size);
	QT_CHECK (data && size == 4 && !memcmp (data, "ab\0\0", 4));

	// a bad pointer is the builtin's error
	f = Open (h, "bad", 2);
	QT_EQ_I (QH_Fails (h, "fwrite", ARGS (F (f), W (0x7FFFFFF0), I (64))), QC_ERR_BUILTIN);
	QT_EQ_I (QH_Fails (h, "fread", ARGS (F (f), W (0), I (64))), QC_ERR_BUILTIN);
	QH_Call (h, "fclose", ARGS (F (f)), NULL);
	QH_Free (h);
}

// FILE_READNL: fgets the whole file (once); the mmap modes: fgets the block,
// RW written back at its size when closed
static void TestWholeFiles (void)
{
	qh_t		*h = Harness ();
	uint8_t		got[8];
	uint32_t	p;
	float		f;

	AddText (h, "data/all.txt", "a\r\nb\n");
	f = Open (h, "all.txt", 4);
	QT_EQ_S (QH_String (h, "fgets", ARGS (F (f))), "a\r\nb\n");
	QT_CHECK (QH_OptString (h, "fgets", ARGS (F (f))) == NULL);
	QH_Call (h, "fclose", ARGS (F (f)), NULL);

	f = Open (h, "all.txt", 5);
	p = QH_Word (h, "fgets", ARGS (F (f)));
	QT_CHECK (p != 0 && QC_ReadMemory (h->vm, p, got, 5) && !memcmp (got, "a\r\nb\n", 5));
	QT_EQ_U (QH_Word (h, "fgets", ARGS (F (f))), p);
	QH_Call (h, "fclose", ARGS (F (f)), NULL);
	QT_EQ_F (Open (h, "none.txt", 5), -1);

	// RW: at least the size asked for; written back at its size
	f = QH_Float (h, "fopen", ARGS (QH_S (h, "rw.bin"), F (6), F (8)));
	p = QH_Word (h, "fgets", ARGS (F (f)));
	QT_EQ_I (QH_Int (h, "fsize", ARGS (F (f))), 8);
	QC_WriteMemory (h->vm, p, "12345678", 8);
	QH_Call (h, "fsize", ARGS (F (f), I (5)), NULL);
	QH_Call (h, "fclose", ARGS (F (f)), NULL);
	QT_EQ_S (FileText (h, "data/rw.bin"), "12345");
	f = Open (h, "rw.bin", 6);
	p = QH_Word (h, "fgets", ARGS (F (f)));
	QT_CHECK (QC_ReadMemory (h->vm, p, got, 5) && !memcmp (got, "12345", 5));
	// writing goes into the block, as far as it goes
	QH_Call (h, "fputs", ARGS (F (f), QH_S (h, "xy")), NULL);
	QH_Call (h, "fseek", ARGS (F (f), I (4)), NULL);
	QH_Call (h, "fputs", ARGS (F (f), QH_S (h, "pqr")), NULL);
	QH_Call (h, "fclose", ARGS (F (f)), NULL);
	QT_EQ_S (FileText (h, "data/rw.bin"), "xy34p");
	QH_Free (h);
}

// fremove and frename in data/: 0, -5 when they can't, -1 for names refused;
// whichpack's pack, "" for a file of its own, null for none
static void TestNames (void)
{
	qh_t	*h = Harness ();

	AddText (h, "data/old.txt", "x");
	QT_EQ_F (QH_Float (h, "frename", ARGS (QH_S (h, "old.txt"), QH_S (h, "new.txt"))), 0);
	QT_EQ_S (FileText (h, "data/new.txt"), "x");
	QT_EQ_F (QH_Float (h, "frename", ARGS (QH_S (h, "old.txt"), QH_S (h, "new.txt"))), -5);
	QT_EQ_F (QH_Float (h, "frename", ARGS (QH_S (h, "new.txt"), QH_S (h, "../up.txt"))), -1);
	QT_EQ_F (QH_Float (h, "fremove", ARGS (QH_S (h, "new.txt"))), 0);
	QT_CHECK (QH_File (h, "data/new.txt", &(size_t){0}) == NULL);
	QT_EQ_F (QH_Float (h, "fremove", ARGS (QH_S (h, "new.txt"))), -5);
	QT_EQ_F (QH_Float (h, "fremove", ARGS (QH_S (h, "/abs"))), -1);

	QH_AddPackedFile (h, "pak0.pak", "maps/start.bsp", "b", 1);
	AddText (h, "readme.txt", "r");
	QT_EQ_S (QH_String (h, "whichpack", ARGS (QH_S (h, "maps/start.bsp"))), "pak0.pak");
	QT_CHECK (QH_OptString (h, "whichpack", ARGS (QH_S (h, "readme.txt"))) != NULL);
	QT_EQ_S (QH_String (h, "whichpack", ARGS (QH_S (h, "readme.txt"))), "");
	QT_CHECK (QH_OptString (h, "whichpack", ARGS (QH_S (h, "none.bsp"))) == NULL);
	QT_CHECK (QH_OptString (h, "whichpack", ARGS (QH_S (h, "../readme.txt"))) == NULL);
	QH_Free (h);
}

// handles: limits.files of them, a closed one's slot reused, bad ones warned
// about; the files left open are closed (and written) when the VM is reset
static void TestHandles (void)
{
	qc_config_t	config;
	qh_t		*h;
	float		a, b;

	QC_DefaultConfig (&config, QC_CSQC);
	config.limits.files = 2;
	h = HarnessWith (&config);
	a = Open (h, "a.txt", 2);
	b = Open (h, "b.txt", 2);
	QT_EQ_F (a, 1000);
	QT_EQ_F (b, 1001);
	QH_ClearWarnings (h);
	QT_EQ_F (Open (h, "c.txt", 2), -1);
	QT_EQ_I (QH_NumWarnings (h), 1);
	QH_Call (h, "fclose", ARGS (F (a)), NULL);
	QT_EQ_F (Open (h, "c.txt", 2), 1000);

	QH_ClearWarnings (h);
	QH_Call (h, "fclose", ARGS (F (5)), NULL);
	QT_CHECK (QH_OptString (h, "fgets", ARGS (F (1002))) == NULL);
	QT_EQ_I (QH_Int (h, "fseek", ARGS (F (999))), -1);
	QT_EQ_I (QH_NumWarnings (h), 3);

	QH_Call (h, "fputs", ARGS (F (b), QH_S (h, "kept")), NULL);
	QT_CHECK (QC_Reset (h->vm));
	QT_EQ_S (FileText (h, "data/b.txt"), "kept");
	QT_CHECK (QH_File (h, "data/c.txt", &(size_t){0}) != NULL);
	QH_Free (h);
}

// what a file to write holds counts against limits.container_bytes
static void TestBudget (void)
{
	qc_config_t	config;
	qh_t		*h;
	uint32_t	p;
	float		f;

	QC_DefaultConfig (&config, QC_CSQC);
	config.limits.container_bytes = 4096;
	h = HarnessWith (&config);
	p = QH_Word (h, "memalloc", ARGS (I (8192)));
	f = Open (h, "big", 2);
	QT_EQ_I (QH_Int (h, "fwrite", ARGS (F (f), W (p), I (1000))), 1000);
	QT_EQ_I (QH_Int (h, "fwrite", ARGS (F (f), W (p), I (8000))), 0);
	QH_Call (h, "fclose", ARGS (F (f)), NULL);
	QT_EQ_I (h->vm->std->container_bytes, 0);
	// reading a whole file too large for it fails
	{
		static uint8_t	large[8192];

		QH_AddFile (h, "data/large", large, sizeof(large));
	}
	QT_EQ_F (Open (h, "large", 4), -1);
	QT_EQ_I (h->host.openfiles, 0);
	f = Open (h, "large", 0);
	QT_EQ_I (QH_Int (h, "fread", ARGS (F (f), W (p), I (8192))), 8192);
	QH_Call (h, "fclose", ARGS (F (f)), NULL);
	QH_Free (h);
}

// without the host's callbacks nothing opens, but files to write still do
static void TestNoHost (void)
{
	qc_builtins_t	*b = QC_BuiltinsStandard (QC_NUMBERING_CSQC);
	qc_asm_t		*a = QA_New ();
	qc_progs_t		*p;
	qc_config_t		config;
	qcvm_t			*vm;
	qc_value_t		args[2], ret;

	QA_Builtin (a, "fopen", 0, -1);
	p = QA_Load (a, QC_FORMAT_FTE32);
	QA_Free (a);
	QC_DefaultConfig (&config, QC_CSQC);
	vm = QC_Create (p, b, &config, NULL, NULL, NULL);
	QC_ReleaseProgs (p);
	if (QT_CHECK (vm != NULL))
	{
		args[0] = QC_ValWord (QC_TempString (vm, "x.txt", 5));
		args[1] = QC_ValFloat (0);
		QT_CHECK (QC_Call (vm, QC_FindFunction (vm, "fopen"), 2, args, &ret) && QC_BitsFloat (ret.w[0]) == -1);
		args[1] = QC_ValFloat (2);
		QT_CHECK (QC_Call (vm, QC_FindFunction (vm, "fopen"), 2, args, &ret) && QC_BitsFloat (ret.w[0]) == 1000);
		QC_Destroy (vm);
	}
	QC_BuiltinsFree (b);
}

int main (void)
{
	TestReadLines ();
	TestSandbox ();
	TestWrite ();
	TestBytes ();
	TestWholeFiles ();
	TestNames ();
	TestHandles ();
	TestBudget ();
	TestNoHost ();
	return QT_Finish ("lib_file", "QuakeC's files: FTE's sandbox, lines, bytes, writing, mmap, names and handles");
}
