# The QuakeC VM

SoftWorld runs QuakeC on a C translation of [qcvm-rs](https://github.com/dsvensson/qcvm-rs), a
re-entrant QuakeC VM hardened for untrusted progs. It lives in `src/qcvm` as the static library
`sw_qcvm`, which needs nothing of the engine but `sw_common`. The server runs its progs on it,
and the client uses it for client-side QuakeC (CSQC).

- **Formats and opcodes:** progs from `fteqcc` for any FTE target: version 6, FTE version 7
  (16- and 32-bit statements), KK7, uHexen2 and QTest, with every opcode FTE runs.
- **Builtins:** about 200 standard builtins that need no engine, under FTE's numbering for
  CSQC, SSQC and menu progs. They cover maths, vectors, strings, `sprintf`, tokenizers, info
  strings, entity searches, hash tables, string buffers, VM memory, JSON and digests.
- **The VM:** re-entrant calls (builtins may call QuakeC), several progs in one VM (FTE's
  multiprogs), QuakeC threads (`sleep`, `fork`), and autocvars.
- **Hardening:** hard limits on every resource. A malformed progs is refused or its bad
  statements are poisoned. A QuakeC error is a return value with a backtrace, never a crash.

What the VM does is specified in [spec](spec), qcvm-rs's specification. Where FTE has a bug, the
VM does otherwise, and [spec/deviations.md](spec/deviations.md) lists those places. The C port's
own differences from qcvm-rs are [below](#the-c-port-and-qcvm-rs).

## Using it

`qcvm.h` is the whole interface. A program is loaded once and may be shared between VMs. A
registry of builtins binds the progs' builtins by number and name. A VM gets the progs, the
registry, a configuration (`QC_DefaultConfig` for CSQC, SSQC or menu progs) and the host's
callbacks (`qc_host_t`: prints, cvars, warnings, entity hooks and so on; NULL ones default).

```c
#include "qcvm.h"

// float() framecount = #500;
static bool Framecount (qcvm_t *vm)
{
	const int	*frames = QC_HostContext (vm);

	QC_ReturnFloat (vm, (float)*frames);
	return true;
}

static void Print (void *ctx, const char *text)
{
	(void)ctx;
	fputs (text, stdout);
}

bool RunInit (const void *data, size_t size)
{
	static int			frames;
	qc_host_t			host = {.print = Print};
	qc_config_t			config;
	qc_loaderror_t		lerr;
	qc_error_t			err;
	qc_progs_t			*progs = QC_LoadProgs (data, size, &lerr);
	qc_builtins_t		*builtins = QC_BuiltinsStandard (QC_NUMBERING_CSQC);
	qcvm_t				*vm;
	qc_value_t			args[3];
	char				text[1024], trace[4096];
	bool				ok;

	QC_BuiltinsSetNumbered (builtins, 500, "framecount", Framecount);
	QC_DefaultConfig (&config, QC_CSQC);
	vm = QC_Create (progs, builtins, &config, &host, &frames, &err);
	QC_ReleaseProgs (progs);		// the VM holds its own reference
	if (!vm)
		return false;

	args[0] = QC_ValFloat (1);
	args[1] = QC_ValWord (QC_TempString (vm, "engine", 6));
	args[2] = QC_ValFloat (1);
	ok = QC_Call (vm, QC_FindFunction (vm, "CSQC_Init"), 3, args, NULL);
	if (!ok)
		printf ("%s\n%s", QC_ErrorText (QC_LastError (vm), text, sizeof(text)),
			QC_BacktraceText (&QC_LastError (vm)->backtrace, trace, sizeof(trace)));
	QC_Destroy (vm);
	QC_BuiltinsFree (builtins);
	return ok;
}
```

A builtin reads its arguments with `QC_Arg*` and sets its result with `QC_Return*`. On failure
it returns false after `QC_Error` (FTE's builtin error, only a warning in developer mode),
`QC_HostError` or `QC_Abort` (FTE's `abort`). Globals and entity fields never move: the host
keeps pointers to them (`QC_Globals`, `QC_Edicts`), and the configuration can give each entity
a header of the host's own before its fields (`entity_header_bytes`). That is how the server's
`edict_t` sits on the VM's entities. The host can also lay the fields out (`host_fields`, FTE's
`QC_RegisterFieldVar`): the main progs' fields of those names move where the host's struct has
them, the rest go after, so one struct reads progs that order their fields differently. Further
progs come in with `QC_AddProgs`, their fields unified with these by name. The host resumes
sleeping threads with `QC_RunThreads` once a frame, and copies its cvars into autocvars with
`QC_SyncAutocvars`.

The specification uses qcvm-rs's Rust names. Their C counterparts are:

| qcvm-rs | C |
|---|---|
| `Program` | `qc_progs_t` (`QC_LoadProgs`, `QC_Progs*`) |
| `Vm`, `VmConfig`, `Limits::x` | `qcvm_t`, `qc_config_t`, `qc_config_t.limits.x` |
| `FteCompat` flags | `qc_config_t.compat` |
| `Host::x` | `qc_host_t.x` |
| `Builtins::standard` | `QC_BuiltinsStandard` |
| `Vm::add_progs`, `Vm::run_threads`, `Vm::sync_autocvars` | `QC_AddProgs`, `QC_RunThreads`, `QC_SyncAutocvars` |

## In SoftWorld

**The server** runs `qwprogs.dat` or NetQuake's `progs.dat` on the VM, with FTE's server
behaviour.

- **Which progs:** as FTE chooses: the game directory's own `progs.dat` or `qwprogs.dat` over
  the base's (id1's and qw's, and the `qwprogs.dat` built in); else `progs.dat` when
  `deathmatch` is 0 (single player and coop) and `qwprogs.dat` when it isn't. `sv_progs` names
  one outright. The header CRC tells them apart: 54730 is QuakeWorld's, anything else acts as
  NetQuake's (FTE's PROG_UNKNOWN).
- **Fields and globals:** the server has its own layout of id's, QuakeWorld's fields in their
  order then NetQuake's `punchangle` and `idealpitch` (`progdefs.h`). A progs' fields move there
  by name (`host_fields`), and its globals are bound by name (FTE's `globalptrs_t`), the server
  keeping those a progs lacks: QuakeWorld's `newmis`, NetQuake's `deathmatch`, `coop` and
  `teamplay`.

- **Builtins:** the id builtins are FTE's, from the standard library. `pr_cmds.c` keeps only
  those that need the engine, and FTE's server versions of `objerror` and entity removal.
  `objerror` prints the entity, removes `self` and aborts the call, and isn't fatal. Removal
  refuses the world and the player slots, and clears id's fields and the classname.
  NetQuake's progs get its `bprint` and `sprint` (no level), `particle` (#48: blood and
  lightning's blood as QuakeWorld's temp entities, as FTE sends them), and `setmodel`'s box for
  every model. FTE's `clientstat` and `globalstat` (#232, #233) add stats from 32 up, sent to
  clients with `FTE_PEXT_CSQC`; `pr_checkextension` says `checkextension` answers.
- **Particles:** FTE's and DarkPlaces' particle builtins, as FTE's server sends them to its
  clients with `FTE_PEXT_CSQC` (`sv_part.c`): `particleeffectnum` (#335) numbers an effect by
  name (the first, effectinfo.<anything>, numbers `effectinfo.txt`'s effects first, as
  QuakeSpasm-Spiked does), the names going out at prespawn and at once for one named later;
  `trailparticles` (#336, the effect first or DarkPlaces' entity first), `pointparticles`
  (#337), `te_particlerain` and `te_particlesnow` (#409, #410), `te_explosion2` (#427, FTE's
  `TE_EXPLOSION2` to clients with `FTE_PEXT_TE_BULLET`, a plain explosion to the rest); and the
  `traileffectnum` and `emiteffectnum` fields, in replacement deltas. MVDs and QTV get none of
  them.
- **checkextension** answers the standard library's extensions and the server's
  `FTE_SV_POINTPARTICLES`, `FTE_PART_SCRIPT`, `FTE_PART_NAMESPACES`,
  `FTE_PART_NAMESPACE_EFFECTINFO`, `DP_ENT_TRAILEFFECTNUM`, `DP_TE_PARTICLERAIN` and
  `DP_TE_PARTICLESNOW` (the `FTE_PART_` ones for the client's scripts, `src/particles`).
- **NetQuake's messages:** what NetQuake's progs write with the `Write` builtins is held until
  a message is whole and written again in QuakeWorld's words (`sv_nqmsg.c`, as FTE's
  `net_preparse.c`): the gunshot gets its count, the intermission each client's view, the cd
  track loses its loop track, the stats are longs; temp entities are multicast where they
  happen, NetQuake's `TE_EXPLOSION2` and `TE_BEAM` as FTE's to the clients with
  `FTE_PEXT_TE_BULLET` (a plain explosion, and nothing, to the rest). A message it doesn't know is dropped with a warning. `EF_MUZZLEFLASH` becomes
  `svc_muzzleflash`, and `punchangle` the client's kicks.
- **NetQuake's players** move as NetQuake moves them (id's `SV_ClientThink` and
  `SV_Physics_Client`), in the world's frame (at least every 0.013 s, FTE's) by the newest
  move each sent, which is read before it; the client doesn't predict them (`PM_NONE`), as with
  FTE's `sv_nqplayerphysics`. A listen server's game of one (`maxclients 1`) holds still while
  its player's menu or console is up, as FTE's `PAUSE_AUTO`.
- **The world** is read-only once the map runs (a write is a warning and skipped).
- **Threads:** those sleeping resume after `StartFrame`.
- **Autocvars** follow their cvars.
- **Add-ons:** `addprogs` loads progs from the game directory.
- **Errors:** a QuakeC error stops the server with the error and its backtrace, from the
  `.lno` file when the compiler wrote one.
- **CSQC:** each map publishes the csprogs named by `sv_csqc_progname` (default
  `csprogs.dat`), in the serverinfo as FTE does: `*csprogs` (its folded MD4), `*csprogssize`,
  and `*csprogsname` when it isn't `csprogs.dat`. A `csprogs.dat` at the root of the game
  directory may be downloaded when `allow_download` is on.

Developer commands: `edict`, `edicts`, `edictcount`, `edictdigest` (a hash per entity, to
compare two builds on a map), `profile`, and `pr_builtins`, the builtins the running progs calls
that the server lacks (with `all`, those it declares).

**The client** finds, downloads, checks and loads a server's csprogs as FTE's client does, and
runs its lifecycle. The CSQC networking (entities, the parse hooks, input) and the drawing
aren't there yet, so `cl_nocsqc` keeps CSQC off by default. With `cl_nocsqc 0`:

- **Download:** the csprogs a server offers is downloaded as `csprogsvers/<checksum>.dat`
  (`cl_download_csprogs`), unless a matching one is here already.
- **Checking:** it must match the size and checksum the serverinfo gives, except in a demo or
  with the server's `anycsqc`.
- **csaddon.dat** loads after it where cheats apply (a demo, `*cheats`, or a local server for
  one player), and on its own without a csprogs.
- **Lifecycle:** `init` and `initents`, `CSQC_Init`, `CSQC_WorldLoaded` (with
  `getentitytoken` over the map's entities) and `CSQC_Shutdown`, across map changes.
  `registercommand` and `CSQC_ConsoleCommand` work, as do the builtins that need neither
  networking nor drawing.
- **Errors:** a QuakeC error prints its backtrace and shuts CSQC down, and the client carries on.

`csqc_builtins` lists the builtins the running csprogs calls that the client lacks, or those of
a progs file (`csqc_builtins csprogsvers/5df265ea.dat`, with `all` for every one it declares).
It is the checklist for the rest of CSQC.

**The menu** is menu QuakeC, run as FTE runs it (`cl_menu.c`): the game directory's
`menu.dat`, else the one built in, compiled from `menu-qc`.

- **Entry points:** `m_init` and `m_shutdown`, `m_draw` each frame the menu has the keys
  (FTE's `vector` of the layout's size, or DarkPlaces' two floats), `m_keydown` and `m_keyup`
  with FTE's key codes and the character typed, `m_toggle` for Escape and `togglemenu`, and
  `m_consolecommand` for the commands it registers. `Menu_InputEvent` gets the keys first (an
  event it takes doesn't reach `m_keydown`), and the mouse's motion while it has the mouse
  captured (`setcursormode`). `time` is the host's realtime. The menu isn't entered again from
  inside its QuakeC (a print it makes draws the screen at once while the console is up).
- **Builtins:** the standard library's, numbered for menus, and the client's: `precache_pic`,
  `iscachedpic`, `drawpic`, `drawcharacter`, `drawrawstring`, `drawstring` (the colors written
  in it, ezQuake's `&cRGB` and FTE's `^`), `stringwidth`, `drawfill`, `drawgetimagesize`,
  `r_uploadimage` (FTE's formats 1, RGBA, and 15 and 16, a byte a pixel and a palette after),
  `r_readimage` (RGBA, also `gfx/conchars` and `gfx/palette.lmp` as 16x16), `shaderforname`
  (the name draws the image its shader's first stage maps), `localsound`, `queueaudio` and
  `getqueuedaudiotime` (sound QuakeC makes, kept whole and counted in its own frames),
  `setkeydest`, `getkeydest`, `setcursormode`, `getcursormode`, `keynumtostring`,
  `stringtokeynum`, `findkeysforcommand`, `getkeybind`, `setkeybind`, `clientstate` (2 only in
  a game), `clipboard_set` (the clipboard, cliptype 0) and SoftWorld's `isfullscreen`. Its files
  (`fopen` and the rest) are FTE's sandbox in the game directory, and `addprogs` loads further
  progs from it (a launcher's games). Its heap is `pr_menu_memsize` (1g, address space committed
  as it is used; 16m on the web and 32-bit builds). Uploaded images are drawn at the size asked.
  So FTE's guest engines run in it: Spike's qcquake and qcquake2. Its 2D is drawn
  1:1 in the layout's pixels, 8 by 8 a character: text takes its rgb as a tint of four bits a
  channel and an alpha below 1 as half transparent, a fill its rgb and alpha as they are,
  pictures neither. It is clipped to the screen, and what it asks for never stops the program.
- **The server list:** FTE's hostcache builtins (611 to 622) over the client's (`cl_slist.c`):
  `gethostcachevalue` (FTE's 0 to 7, and SoftWorld's from 100: whether it scans, the sweep and
  sweeps, pings sent and to send, servers alive, dead and described, the list's generation, the
  sources, the marked sources' servers, the scans asked for), `gethostcacheindexforkey` (FTE's
  names, `player<N>`, `state` for cached, alive and dead, `qtv` for its game's stream on QTV as
  `qtvplay` takes it, and any serverinfo key), `gethostcachestring` and `gethostcachenumber` (a
  player as FTE spells one, its team and "b" for a bot after), the masks as FTE tests them (in order, ANDed, or ORed
  with mask 512), `sethostcachesort` (and its flag 8, a key after the others), `resorthostcache` and
  `refreshhostcache`. SoftWorld's own, by name: `gethostcacheindexforaddress`,
  `refreshhostcacheentry` (a server asked again ahead of the rest), `hostcacheinsource`,
  `gethostcachesource`, `sethostcachesourcemark`, `addhostcachesource`, `removehostcachesource` and
  `addhostcacheserver`. A server's index holds from one `m_draw` to the next: the list the engine
  finds is taken before it.
- **Errors:** a QuakeC error prints its backtrace and shuts the menu down; a game directory's
  menu.dat gives way to the built-in one. `menu_restart` loads the menu again, and
  `menu_builtins` lists the builtins it calls that the client lacks.

## Tests

`ctest` runs the VM's tests, the ports of qcvm-rs's suite:

| Test | Checks |
|---|---|
| `qc_loader` | every format, the load-time rewrites, malformed and corrupted progs; the built qwprogs.dat and menu.dat |
| `qc_memory` | memory, entities, the host's header, strings and their collection, the heap |
| `qc_opcodes` | every opcode, in 16- and 32-bit statements |
| `qc_vm` | calls, re-entry, errors and backtraces, the limits, abort, tracing, a longjmp out |
| `qc_fuzz` | random programs and hostile builtin arguments, and the traced and plain loops agree |
| `qc_lib_<area>` | the standard builtins, area by area |
| `qc_fixtures` | QuakeC fixtures compiled with `fteqcc` for FTE and vanilla targets |
| `qc_multiprogs`, `qc_threads` | several progs in one VM; `sleep` and `fork` |
| `qc_csprogs` | KTX's csprogs against a stub CSQC engine |

Some need something from outside and are skipped without it:

| | |
|---|---|
| `FTE_QCVM` | FTE's standalone `qcvm` runner: the fixtures' output is compared with its output |
| `QCVM_CSPROGS` | a KTX `csprogs.dat` for `qc_csprogs` (and `qc_loader`'s KTX test); `QCVM_PROFILE=1` also profiles it |
| `QC_FUZZ_ITERS` | how many random programs the fuzz tests run (256 by default) |

## The C port and qcvm-rs

The port follows qcvm-rs module by module, and its tests are qcvm-rs's. The C design:

- **Memory:** region S holds the strings, globals, local stack and added progs, E the
  entities, and H the heap. Each region is address space reserved up front and committed as it
  grows, so nothing moves and the host's pointers stay good. QuakeC's addresses and string and
  function encodings are qcvm-rs's.
- **Errors:** a failure is `false` and `QC_LastError`, with a backtrace. Out of memory is a VM
  error, not an exit.
- **The interpreter:** a `switch`, compiled twice (plain, and traced for tracing and profiling).
  Operands are relocated at load and checked there, so running needs no checks on them.
- **Numbers:** floats are formatted by an exact formatter of their own (`qc_dtoa.c`), as glibc
  prints them, and strings are read in the C locale, so the output is the same everywhere.

It differs from qcvm-rs here:

- **Hooks:** `QC_Spawn` and `QC_Remove` always run the host's hooks, where qcvm-rs's host
  calls skip them. `on_remove` can refuse a removal (FTE's `entcanfree`).
- **API additions:**
  - `QC_ClaimEdict` marks a slot in use, for entities the host owns (the server's players).
  - `QC_CommitEdicts` gives a host that reaches entities directly their memory up front.
  - `QC_SyncAutocvar` updates the autocvars of one cvar.
  - The host's header in each entity.
- **`remove_clears`** clears every word of a vector field, where qcvm-rs clears one.
- **Static strings** may be the host's own text, read where it is (`QC_HostString`: id's
  `PR_SetString`, FTE's engine strings), as well as interned copies.
- **A longjmp out of the VM** (the engine's errors) is followed by `QC_Abandon`, which puts the
  VM back as if nothing ran. qcvm-rs instead poisons a VM a panic went through.
- **Engine code in an added progs:** when a builtin runs from an added progs, the shared globals
  are copied into the main progs around the call, so engine code reading the main progs'
  `self` sees the right one.
- **`QC_AddProgs`** relocates each global word once, however many definitions share it.
  qcvm-rs relocates by definition, so a string constant under two names would be moved twice.

SoftWorld's hosts differ from FTE's here:

- **Autocvars without a cvar:** they keep the progs' value. FTE creates a cvar for them, and
  SoftWorld's cvars are all the engine's own.
- **CSQC init order:** the add-on's `init` runs as it is added, after the csprogs' `init`, where
  FTE loads both first. So the csaddon's autocvars follow its `init`.
- **Simple CSQC** (FTE running a `progs.dat` with `CSQC_DrawHud` as CSQC) isn't there.

## Credits and license

The VM is a translation of qcvm-rs, and the files in [spec](spec) are qcvm-rs's specification,
licensed MIT OR Apache-2.0 as their headers say. qcvm-rs started as a port of
[QCVM](https://github.com/erysdren/QCVM) by erysdren (MIT). FTEQW's behaviour is the
reference for the instruction set, the builtins and the CSQC machinery. The C code is part of
SoftWorld, under its license.
