<!-- SPDX-License-Identifier: MIT OR Apache-2.0 -->

# FTE utility builtins: catalogue and semantics

This document describes FTEQW behaviour, written from scratch in our own words after studying FTE
as a behavioural specification. No FTE (GPL) code is reproduced here; `file:line` references into
`fteqw/engine/` are pointers for behaviour lookup only.

Where this crate deliberately deviates from FTE, see [deviations.md](deviations.md); the
deviations are also noted where they apply below. String builtins are specified in
[strings.md](strings.md).

Builtins fall into four classes:

- **A**: needs only the VM's state.
- **A\***: needs only the VM's state, but reads well-known fields (`.origin`, `.mins`, `.maxs`,
  `.solid`, `.flags`, `.angles`, `.ideal_yaw`, `.yaw_speed`, `.idealpitch`, `.pitch_speed`,
  `.gravitydir`).
- **B**: a trivial hook into the host.
- **C**: bound to the engine; not provided by qcvm.

## Conventions

- Entity references are indices. An out-of-range entity gives a "Bad entity index" warning, and
  the world is used instead.
- There are up to eight parameter slots of three words. argc is visible, so builtins detect
  optional parameters by it.
- Variadic string builtins concatenate their arguments (at most eight, about 64 KiB): `print`,
  `dprint`, `error`, `objerror`, `localcmd`, `cprint`, `stov`, the tails of `strpad`, `strconv` and
  `infoadd`, `crc16`, `digest_hex`, `strftime`, the names of `externset` and `externvalue`, and
  `instr`.
- Returned strings are temp strings (`FTE_QC_PERSISTENTTEMPSTRINGS`). Strings stored in VM
  containers (hash table values, string buffer entries) are copied. A temp string made from null
  is null (0); one made from `""` is a non-null empty string.
- There are two error levels:
  - a **builtin error** (`PR_BIError`): in developer mode a warning and a zeroed return value,
    otherwise a trace and an abort (a fatal `VmError` in qcvm);
  - a **run warning**: a message and a trace; execution continues.

## Numbered builtins

The class A, A\* and B builtins visible to CSQC, by number.

| # | Builtin | Signature | Class | Behaviour |
|---|---|---|---|---|
| 1 | `makevectors` | `void(vector)` | A | writes the globals `v_forward`, `v_right` and `v_up` (fatal if missing); see [Vectors and angles](#vectors-and-angles) |
| 6 | `breakpoint` | `void()` | A | the run warning "break statement" and a trace |
| 7 | `random` | `float()`, `float(max)`, `float(min, max)` | A | *r* = (*k* + 0.5) / 32768 for a 15-bit *k*, so strictly inside (0, 1); with one argument *r* · max, with two min + *r* · (max − min) |
| 9 | `normalize` | `vector(vector)` | A | the length is computed in float; 0 → `'0 0 0'`; otherwise v · (1 / length) |
| 10 | `error` | `void(string, ...)` | B | prints the message and a trace; fatal |
| 11 | `objerror` | `void(string, ...)` | B | dumps `self`'s fields and the message, frees `self`; fatal in CSQC |
| 12 | `vlen` | `float(vector)` | A | √(v · v) |
| 13 | `vectoyaw` | `float(vector, optional entity ref)` | A / A\* | see [Vectors and angles](#vectors-and-angles) |
| 14 | `spawn` | `entity()` | A | see [Entity storage](#entity-storage) |
| 15 | `remove` | `void(entity)` | A | see [Entity storage](#entity-storage) |
| 18 | `find` | `entity(entity start, .string fld, string match)` | A | see [Entities and searching](#entities-and-searching) |
| 22 | `findradius` | `entity(vector org, float rad, optional .entity chain)` | A\* | see [Entities and searching](#entities-and-searching) |
| 25 | `dprint` | `void(string, ...)` | B | prints only in developer mode |
| 26 | `ftos` | `string(float)` | A | [strings.md](strings.md) |
| 27 | `vtos` | `string(vector)` | A | [strings.md](strings.md) |
| 28 | `coredump` | `void()` | B | dumps the globals and every entity (to a log or file) |
| 29, 30 | `traceon`, `traceoff` | `void()` | A | statement tracing on / off |
| 31 | `eprint` | `void(entity)` | B | `"Entity N:"` and a savegame-style dump of its fields |
| 36 | `rint` | `float(float)` | A | (int)(f + 0.5) if f > 0, else (int)(f − 0.5); overflow gives `INT_MIN` (x86) |
| 37, 38 | `floor`, `ceil` | `float(float)` | A | C's `floor` and `ceil` |
| 43 | `fabs` | `float(float)` | A | the absolute value |
| 45 | `cvar` | `float(string)` | B | a cvar's numeric value (host) |
| 46 | `localcmd` | `void(string, ...)` | B | appends to the command buffer (host) |
| 47 | `nextent` | `entity(entity)` | A | the first in-use entity after the argument, else the world |
| 49 | `changeyaw` | `void()` | A\* | see [Vectors and angles](#vectors-and-angles) |
| 51 | `vectoangles` | `vector(vector fwd, optional vector up)` | A | see [Vectors and angles](#vectors-and-angles) |
| 60, 61, 62 | `sin`, `cos`, `sqrt` | `float(float)` | A | C's double maths, rounded to float; radians |
| 63 | `changepitch` | `void(entity)` | A\* | like `changeyaw` on `angles_x`, `.idealpitch` and `.pitch_speed`; **FTE acts on `self` and ignores the argument** (qcvm acts on the argument) |
| 65 | `etos` | `string(entity)` | A | `"entity N"` |
| 72 | `cvar_set` | `void(string, string)` | B | host |
| 81 | `stof` | `float(string)` | A | [strings.md](strings.md) |
| 91 | `randomvec` | `vector()` | A | draws components (*k* / 32767) · 2 − 1 until ‖v‖² < 1 |
| 93 | `registercvar` | `float(string name, string def, optional float flags)` | B | 0 if the cvar existed, 1 if it was created; FTE reads the default only with three arguments (qcvm always passes it on) |
| 94, 95 | `min`, `max` | `float(float, float, ...)` | A | two arguments: a comparison; three to eight: replace on strictly smaller / larger; fewer than two: a builtin error |
| 96 | `bound` | `float(float min, float val, float max)` | A | val > max → max; else val < min → min; else val (max wins; a NaN val is returned) |
| 97 | `pow` | `float(float, float)` | A | C's `pow` |
| 98 | `findfloat`, alias `findentity` | `entity(entity, .__variant, __variant)` | A | a raw 32-bit comparison; see [Entities and searching](#entities-and-searching) |
| 99 | `checkextension` | `float(string)` | B | looked up in the host's list, exactly (case-sensitive) |
| 102 | `anglemod` | `float(float)` | A | ± 360 until in [0, 360) (computed with a remainder; NaN passes through) |
| 114–119 | `strlen`, `strcat`, `substring`, `stov`, `strzone`, `strunzone` | | A | [strings.md](strings.md) |
| 201 | `externcall` | `__variant(float prnum, string fn, ...)` | A | see [Introspection](#introspection) |
| 202 | `addprogs` | `float(string)` | B | loads further progs into the VM (multiprogs); the host supplies them |
| 203 | `externvalue` | `__variant(float prnum, string name)` | A | see [Introspection](#introspection) |
| 204 | `externset` | `void(float prnum, __variant v, string name)` | A | see [Introspection](#introspection) |
| 210, 212 | `fork`, `sleep` | SSQC; also in FTE's CSQC table | A | QuakeC threads, resumed by `time` |
| 211 | `abort` | `void(optional __variant ret)` | A | unwinds every QuakeC frame to the engine's entry, returning `ret` |
| 218 | `bitshift` | `float(float n, float q)` | A | on integers: q ≥ 0 → n << q; q < 0 → n >> −q, arithmetic. FTE masks the count to 5 bits; qcvm shifts everything out at 32 or more |
| 221–230 | `strstrofs`, `str2chr`, `chr2str`, `strconv`, `strpad`, `infoadd`, `infoget`, `strncmp` (`strcmp`), `strcasecmp`, `strncasecmp` | | A | [strings.md](strings.md) |
| 231 | `calltimeofday` | `void()` | A | if a QuakeC function `timeofday` exists, calls it with (sec, min, hour, day 1–31, **month 0–11**, year, the temp string `"%a %b %d, %H:%M:%S %Y"`) in local time |
| 235 | `rotatevectorsbyangle` | `void(vector)` | A | see [Vectors and angles](#vectors-and-angles) |
| 236 | `rotatevectorsbyvectors` | `void(vector f, vector r, vector u)` | A | see [Vectors and angles](#vectors-and-angles) |
| 245 | `mod` | `float(float a, float n)` | A | n == 0 → the run warning "mod by zero" and 0; else a − n · trunc(a / n) (with the sign of a) |
| 259–262 | `stoi`, `itos`, `stoh`, `htos` | | A | [strings.md](strings.md) |
| 287–292 | `hash_createtab`, `hash_destroytab`, `hash_add`, `hash_get`, `hash_delete`, `hash_getkey` | | A | see [Hash tables](#hash-tables) |
| 293 | `hash_getcb` | | A | a no-op in FTE; see [Hash tables](#hash-tables) |
| 294 | `checkcommand` | `float(string)` | B | 1 a command, 2 an alias, 3 a cvar, 0 none |
| 295 | `argescape` | `string(string)` | A | quotes like `sprintf`'s `%S` ([strings.md](strings.md)) |
| 338 | `cprint` | `void(string, ...)` | B | centerprint |
| 339 | `print` | `void(string, ...)` | B | prints unconditionally |
| 349 | `isdemo` | `float()` | B | 0, 1, or 2 (MVD) |
| 350 | `isserver` | `float()` | B | a host flag |
| 352 | `registercommand` | `void(string)` | B | routes the command to `CSQC_ConsoleCommand` |
| 353 | `wasfreed` | `float(entity)` | A | 1 if the slot is not in use |
| 384–390 | `memalloc`, `memfree`, `memcpy`, `memfill8`, `memgetval`, `memsetval`, `memptradd` | | A | see [VM memory](#vm-memory) |
| 400 | `copyentity` | `entity(entity from, optional entity to)` | A | see [Entities and searching](#entities-and-searching) |
| 402, 403 | `findchain`, `findchainfloat` | | A | see [Entities and searching](#entities-and-searching) |
| 432 | `vectorvectors` | `void(vector)` | A | see [Vectors and angles](#vectors-and-angles) |
| 441, 442 | `tokenize`, `argv` | | A | [strings.md](strings.md) |
| 448 | `cvar_string` | `string(string)` | B | host |
| 449, 450 | `findflags`, `findchainflags` | | A | see [Entities and searching](#entities-and-searching) |
| 459 | `edict_num` | `entity(float)` | A | n ≥ `num_edicts` or negative → the world; else entity n, even if free (never allocated → the world) |
| 460–469 | `buf_create`, `buf_del`, `buf_getsize`, `buf_copy`, `buf_sort`, `buf_implode`, `bufstr_get`, `bufstr_set`, `bufstr_add`, `bufstr_free` | | A | see [String buffers](#string-buffers) |
| 471–475 | `asin`, `acos`, `atan`, `atan2(y, x)`, `tan` | | A | radians, double maths |
| 476–481, 484, 485 | `strlennocol`, `strdecolorize`, `strftime`, `tokenizebyseparator`, `strtolower`, `strtoupper`, `strreplace`, `strireplace` | | A | [strings.md](strings.md) |
| 482 | `cvar_defstring` | `string(string)` | B | host |
| 494 | `crc16` | | A | [strings.md](strings.md) |
| 495 | `cvar_type` | `float(string)` | B | bits: 1 exists, 2 archived, 4 private, 8 engine, 16 has a description, 32 read-only |
| 496–500 | `numentityfields`, `entityfieldname`, `entityfieldtype`, `getentityfieldstring`, `putentityfieldstring` | | A | see [Field reflection and entity text](#field-reflection-and-entity-text) |
| 510, 511 | `uri_escape`, `uri_unescape` | | A | [strings.md](strings.md) |
| 512 | `num_for_edict` | `float(entity)` | A | the index |
| 514–516 | `tokenize_console`, `argv_start_index`, `argv_end_index` | | A | [strings.md](strings.md) |
| 517 | `buf_cvarlist` | `void(strbuf, string pat, string antipat)` | B | enumerates the host's cvars |
| 518 | `cvar_description` | `string(string)` | B | host |
| 519 | `gettime` | `float(optional float type)` | A / B | see [Time](#time); FTE's CSQC name is `gettimef` |
| 529 | `loadfromdata` | `void(string)` | A | see [Field reflection and entity text](#field-reflection-and-entity-text) |
| 530 | `loadfromfile` | `void(string)` | B | a file-reading hook |
| 532 | `log` | `float(float v, optional float base)` | A | ln(v) (in double); with a base, ln(v) / ln(base) |
| 535 | `buf_loadfile` | `float(string, strbuf)` | B | a file-reading hook; one entry per line; 1 if the file was readable |
| 537 | `bufstr_find` | `float(strbuf, string match, float rule, float start, float step)` | A | see [String buffers](#string-buffers) |
| 605 | `callfunction` | `void(..., string fn)` | A | the last argument is a function name; calls it with the preceding arguments (argc − 1); a missing function does nothing |
| 607 | `isfunction` | `float(string)` | A | 1 if the function is defined |
| 613 | `parseentitydata` | `float(entity, string, optional float ofs)` | A | see [Field reflection and entity text](#field-reflection-and-entity-text) |
| 627 | `sprintf` | | A | [strings.md](strings.md) |
| 639 | `digest_hex` | | A | [strings.md](strings.md) |

The engine builtins (class C) have the numbers 2, 3, 4, 8, 16, 19, 20, 32, 34, 35, 40, 41, 48, 64,
67, 68, 69, 74–77, 80, 90, 92, 110–113, 177, 200, 207, 215–217, 219, 237–240, 242, 244, 263–286,
300–337, 340–348, 351, 354–379, 391–394, 404–431, 433–439, 443–447, 451, 452, 457, 483, 486–493,
501–504, 513, 520, 521, 531, 533, 534, 536, 540–542, 603, 604, 606, 608–626, 628–632 and 650–654.

## Builtins resolved by name

These CSQC builtins are declared `#0` and bound by name.

| Builtin | Signature | Class |
|---|---|---|
| `removeinstant` | `void(entity)` | A |
| `find_list` | `entity*(.__variant fld, __variant match, int type = EV_STRING, __out int count)` | A |
| `findradius_list` | `entity*(vector org, float rad, __out int count, int sort = 0)` | A\* |
| `logarithm` | `float(float v, optional float base)` | A |
| `checkbuiltin` | `float(__variant funcref)` | A |
| `createbuffer` | `void*(int bytes)` | A |
| `strtrim` | `string(string)` | A |
| `ftoi` | `int(float)` | A |
| `itof` | `float(int, optional float shift, float mask = 24)` | A |
| `crossproduct` | `vector(vector, vector)` | A |
| `memstrsize` | `float(string)` | A |
| `entityprotection` | `float(entity e, float nowreadonly)` | A |
| `findentityfield` | `float(string)` | A |
| `entityfieldref` | `field_t(float)` | A |
| `generateentitydata` | `string(entity)` | A |
| `digest_ptr` | `string(string alg, void *data, int len)` | A |
| `cvars_haveunsaved` | `float()` | B |

Every other `#0` builtin is class C, including `getplayerkeyfloat`, which csprogs uses. In FTE's
catalogue, `Readint64` differs in case from the engine's name, and `#0:gettime` does not resolve
in CSQC (the engine calls it `gettimef`); qcvm resolves it as an alias.

### In FTE's CSQC table but not in the catalogue

Class A:

| Builtin | Behaviour |
|---|---|
| `argc` `float()` | the number of tokens ([strings.md](strings.md)) |
| `anglesub(a, b)` | a − b wrapped into [−180, 180] (looping while > 180 or < −180, so ±180 survive) |
| `instr(s1, s2, ...)` | a string reference pointing *into* s1 at the first occurrence of the concatenated rest, or null |
| `externrefcall(prnum, funcref, ...)` | see [Introspection](#introspection) |
| `ftou`, `utof` | the unsigned counterparts of `ftoi` and `itof` |
| `memrealloc(ptr, size)` | a new block with the old block's contents (the rest zeroed); the old block is freed; a null `ptr` allocates |
| `memcmp(a, b, size, optional aofs, optional bofs)` | compares VM memory; FTE's implementation swaps the two offsets, qcvm applies the fourth argument to `a` |
| `base64encode(ptr, bytes)` | a temp string |
| `base64decode(string, __out int bytes)` | a heap block (freed with `memfree`); the length goes to the second parameter |
| `gettimed` | the time as a double |
| `json_parse`, `json_free` (= `memfree`), `json_get_value_type`, `json_get_name`, `json_get_integer`, `json_get_float`, `json_get_string`, `json_find_object_child`, `json_get_length`, `json_get_child_at_index` | see [JSON](#json) |

SSQC only, class A: `modulo` (`#0`, the same as `mod`), `builtin_find` #100, `respawnedict`.

Menu only, class A: `altstr_*` 82–85, `etof` 79, `ftoe` 80, `validstring` 81 (a non-null
reference), `crash` 72 (fatal `"<name> called"`), `stackdump` 73.

## Semantics

### Vectors and angles

**`makevectors(a)`**: pitch *p* (positive is down), yaw *y* and roll *r* in degrees, with *sp*,
*cp*, *sy*, *cy*, *sr*, *cr* their sines and cosines. Writes the globals:

- `v_forward` = (cp · cy, cp · sy, −sp)
- `v_right` = (−sr · sp · cy + cr · sy, −sr · sp · sy − cr · cy, −sr · cp)
- `v_up` = (cr · sp · cy + sr · sy, cr · sp · sy − sr · cy, cr · cp)

**`vectoyaw(v)`**: x = y = 0 → 0; otherwise the yaw atan2(y, x) in degrees, **truncated to an
integer**, + 360 if negative (whole degrees in [0, 360)). With a reference entity whose
`.gravitydir` is non-zero: *up* = −normalize(gravitydir); *a* = normalize(a vector perpendicular
to *up*), found by projecting out the axis of smallest magnitude; *b* = *up* × *a*; then x = v · *a*
and y = v · *b*.

**`vectoangles(f, optional up)`**: with `r_meshpitch -1`, so **positive pitch is up**; not
truncated.

- If f.x = f.y = 0: pitch 90 if f.z > 0, else 270 (the zero vector included); yaw 0 without *up*,
  and with *up* atan2(−up.y, −up.x) if f.z > 0, else atan2(up.y, up.x); roll 0.
- Otherwise yaw = atan2(f.y, f.x) and pitch = atan2(f.z, √(f.x² + f.y²)). With *up*,
  roll = −atan2(up · L, up · U), where P = −pitch (in radians), L = (−sin y, cos y, 0) and
  U = (sin P · cos y, sin P · sin y, cos P).
- All components are converted to degrees, + 360 if negative. `vectoangles('0 0 0')` =
  `'270 0 0'`, `vectoangles('0 0 1')` = `'90 0 0'`.

**`vectorvectors(dir)`**: f = `v_forward` = normalize(dir) (0 stays 0). `v_right` is
(0, −1, 0) if f.x = f.y = 0 and f.z ≠ 0, 0 if f is 0, and normalize(f.y, −f.x, 0) otherwise.
`v_up` = `v_right` × `v_forward`.

**`crossproduct`**: the standard cross product.

**`rotatevectorsbyangle(ang)`**: M has the rows [`v_forward`, −`v_right`, `v_up`]; T has the rows
[F, −R, U] from makevectors(ang) with the **pitch negated**; N = T · M (row-major); then
`v_forward` = N₀, `v_right` = −N₁, `v_up` = N₂.

**`rotatevectorsbyvectors(f, r, u)`**: B has the rows [f, −r, u]; T has the rows [`v_forward`,
−`v_right`, `v_up`]; N = T · B, written back the same way.

**`changeyaw()`**, on `self` (with a zero `.gravitydir`):

1. cur = anglemod16(`angles_y`), where anglemod16(a) = (360 / 65536) · ((int)(a · 65536 / 360) &
   65535);
2. if cur == `ideal_yaw`, return;
3. move = ideal − cur; if ideal > cur and move ≥ 180, move −= 360; else if move ≤ −180, move +=
   360;
4. clamp move to ±`yaw_speed`;
5. `angles_y` = anglemod16(cur + move).

With a non-zero `.gravitydir`, the same turn happens in a frame aligned with the surface, with
pitch and roll 0 in that frame.

**`changepitch(e)`**: the same on `angles_x`, `.idealpitch` and `.pitch_speed`. FTE uses `self`;
qcvm uses `e`.

### Scalar maths

`sin`, `cos`, `tan`, `asin`, `acos`, `atan`, `atan2(y, x)`, `sqrt`, `pow`, `floor`, `ceil` and
`fabs` compute in double and round to float. `rint`, `min`, `max`, `bound`, `mod`, `bitshift` and
`log` are described in the [table](#numbered-builtins).

### Random numbers

The PRNG belongs to the VM (seedable, deterministic) and draws 15 bits at a time. `random` and
`randomvec` are described in the [table](#numbered-builtins), the `RAND*` opcodes in
[vm.md](vm.md).

### Hash tables

Buckets are chained, and new entries go to the **head** of their chain. Handles count from 1;
handle 0 is the special `gamestate` table (256 buckets, default type `EV_STRING`, kept across
maps, not owned by any progs, cannot be destroyed), which qcvm keeps for the life of the VM. An
invalid handle is a builtin error.

- **`hash_createtab(n, optional type)`**: n < 4 → 64 buckets; type 0 or omitted → **`EV_VECTOR`
  (3)**. Returns the first free handle ≥ 1, or 0 on failure.
- **`hash_add(t, key, value, optional typeandflags)`**: the type is `flags & 0xFF` (0 → the
  table's default); `HASH_REPLACE` = 256, `HASH_ADD` = 512. An empty key is ignored. Unless
  `HASH_ADD` is set without `HASH_REPLACE`, the first (newest) existing entry for the key is
  removed — only that one. `EV_STRING` values are copied as text; other types copy three raw
  words, so a temp string stored under another type dangles once it is collected. Keys are exact
  and case-sensitive.
- **`hash_get(t, key, optional deflt = '0 0 0', optional requiretype = 0, optional index = 0)`**:
  walks the entries for the key newest first, skipping those whose type differs from a non-zero
  `requiretype`, and skipping the first `index` matches. A string is returned as a temp copy (with
  words 1 and 2 zero), other types as their three raw words; nothing found → `deflt`.
- **`hash_delete(t, key)`**: removes the newest entry for the key, whatever its type, and returns
  its value (zeros if there is none).
- **`hash_getkey(t, idx)`**: enumerates buckets 0 to n − 1, newest first within a bucket; returns a
  temp copy of the key, or null.
- **`hash_destroytab(t)`**: only if the table is owned.
- **`hash_getcb`**: a no-op in FTE; qcvm calls the callback once per entry, taking a snapshot of
  the entries first.

### String buffers

A buffer is a vector of optional strings: its size is the highest set index + 1, and holes are
null. Handles count from 1. FTE ignores invalid handles, and then `buf_getsize`, `buf_implode` and
`bufstr_add` leave the return value untouched; qcvm returns 0 or null.

- **`buf_create(optional type = "string", optional flags = 1)`**: a type other than `"string"`
  (case-insensitive) → −1; flag bit 1 means "saved in savegames".
- **`buf_del`**, **`buf_getsize`**.
- **`buf_copy(from, to)`**: both must be valid and different; `to` is cleared, then receives a deep
  copy, holes included.
- **`buf_sort(b, prefixlen, backward)`**: **compacts the holes first**, then sorts with `strncmp`
  on the first `prefixlen` bytes (≤ 0: unlimited), descending if `backward`; not stable.
- **`buf_implode(b, glue)`**: joins the non-null entries; the glue goes before an entry only if the
  output so far is non-empty.
- **`bufstr_get(b, i)`**: out of range or a hole → null, else a temp copy.
- **`bufstr_set(b, i, s)`**: i > 1,048,576 → a warning, and nothing happens (qcvm's limit is
  `Limits::string_buffer_entries`); otherwise stores a copy, and size = max(size, i + 1).
- **`bufstr_add(b, s, ordered)`**: i = size if `ordered` ≠ 0, else the first hole or size; stores
  a copy and returns i.
- **`bufstr_free(b, i)`**: makes a hole; the size is unchanged.
- **`bufstr_find(b, pattern, rule, start = 0, step = 1)`**: start < 0 or step ≤ 0 → −1. Walks
  i = start, start + step, … while i < size, skipping holes, and returns the first matching index,
  or −1. Rules: 1 exact, 2 prefix, 3 suffix, 4 substring, 0 or 5 wildcard (`?` is any one
  character, `*` a run that does not cross `/` or `\`; ASCII case-insensitive).

### VM memory

- **`memalloc(n)`**: n = 0 → 1 byte; n < 0 or n > 16 MiB → 0 and a builtin error. The memory is
  zero-filled.
- **`memfree(p)`**: 0 does nothing.
- **`memcpy(d, s, n, [sofs, dofs])`**: n < 0 is a builtin error, 0 does nothing; overlapping
  ranges are safe; the source range must be valid (else a builtin error).
- **`memfill8(d, v, n, [dofs])`**: fills with the low byte of v.
- **`memgetval(p, ofs)`, `memsetval(p, ofs, v)`**: the address is p + ofs · 4 (**words**). A
  non-integer ofs warns; an address outside VM memory is a fatal run error; a misaligned one warns.
  FTE computes the address in floating point; qcvm uses exact integer arithmetic.
- **`memptradd(p, ofs)`**: p + ofs **bytes**; an ofs that is not an integer, not a multiple of 4,
  or negative is a builtin error.
- **`createbuffer(n)`**: n ≤ 0 → 0; otherwise a zeroed, garbage-collected temp buffer of n + 1
  bytes (a temp handle: it cannot be freed and is not scanned for roots).

### JSON

`json_parse` builds a flattened tree of nodes in the VM heap and returns a pointer to the root, or
0; `json_free` is `memfree`. The signatures are inferred from FTE's behaviour.

- A node is 16 bytes:

  ```c
  struct { int type; string name; union { struct { int childptr; int count; }; double num; string str; }; }
  ```

- Types: 0 string, 1 number, 2 object, 3 array, 4 true, 5 false, 6 null.
- `json_get_value_type` and `json_get_name` read a node's type and name.
- `json_get_integer`, `json_get_float`: numbers and booleans are converted, strings parsed with
  `atoi`/`atof`; anything else gives 0.
- `json_get_string`: the text of a string node, else null.
- `json_get_length`, `json_get_child_at_index`: the children of an object or array (out of range:
  0).
- `json_find_object_child`: the first child whose name matches exactly, or 0.

FTE's parser is lenient (comments, trailing commas, unquoted words, keys used verbatim); qcvm
accepts only strict JSON.

### Entities and searching

- Searches cover entities 1 to `num_edicts` − 1 and skip free slots; the world is never returned
  or chained, except as the terminator.
- Chains are built by prepending, so the head is the highest-numbered match. The tail's chain
  field is the world; the chain fields of entities that do not match are left alone.
- An invalid field offset is a builtin error.
- **`find(start, .string f, s)`**: scans from start + 1. A null or empty `s` matches null or empty
  fields (with a developer warning); otherwise the bytes must be equal, and a null field never
  matches. The world if nothing matches.
- **`findfloat`, `findentity`**: a **raw 32-bit** comparison (−0 ≠ +0, NaN matches its own
  bits). FTE requires exactly three arguments.
- **`findflags`**: (int)field & (int)v ≠ 0.
- **`findchain(.string f, s, optional .entity chainf = .chain)`**: **skips null fields even when
  `s` is `""`**; otherwise exact.
- **`findchainfloat`**: float equality (−0 = +0, NaN never matches).
- **`findchainflags`**: as `findflags`.
- **`findradius(org, rad, optional chainf = .chain)`**: skips entities with `.solid == 0` unless
  `.flags & 16384` (`FL_FINDABLE_NONSOLID`). The distance is measured to the centre of the
  bounding box, origin + (mins + maxs) / 2, and the entity is included if d² ≤ rad².
- **`find_list(f, match, type = EV_STRING, out count)`**: a temp int array of entity references,
  0-terminated, with the count written to the fourth parameter. For strings a null match means
  `""`. Skips free **and read-only** entities; a null field never matches. Floats and doubles
  compare by value, int64 and vectors per component, other types raw. A bad field or type → 0,
  with count 0.
- **`findradius_list`**: the filter of `findradius`, as a temp 0-terminated array; `sort` is
  ignored. FTE measures a different distance here; qcvm uses the `findradius` test.
- **`nextent`, `edict_num`, `num_for_edict`, `wasfreed`**: see the [table](#numbered-builtins).
- **`copyentity(from, optional to)`**: without `to`, spawns one. A free source, a free or
  read-only destination, or different field sizes are builtin errors. Copies **every** field word
  and returns `to`.
- **`entityprotection(e, ro)`**: a free `e` is an error; values other than 0 and 1 are returned
  but not applied. FTE returns the **new** value; qcvm returns the previous one, as documented.

### Field reflection and entity text

The field table is every field def; vectors appear four times (`v`, `v_x`, `v_y`, `v_z`), as the
progs' field defs already contain them.

- **`numentityfields`**: the number of fields.
- **`findentityfield(name)`**: the index of the first exact match, or 0.
- **`entityfieldref(i)`**: the field offset, or 0.
- **`entityfieldname(i)`**: a temp copy of the name, or null.
- **`entityfieldtype(i)`**: the `EV_*` type, or 0.
- **`getentityfieldstring(i, e)`**: out of range → null; **null for default values** (an all-zero
  vector, a zero word, 255 for `dimension_solid` and `dimension_hit`). Otherwise, by type:

  | Type | Text |
  |---|---|
  | string | the string, with newline, `"` and `\` escaped by a backslash |
  | float, double | `%i` if integral, else `%f` |
  | vector | `%i %i %i` if all components are integral, else `%g %g %g` |
  | entity | its index |
  | function | `progsnum:name` (`0:name` in qcvm) |
  | field | its name |
  | int | `%d`; uint, int64 and uint64 likewise |
  | pointer | `%#x` |

- **`putentityfieldstring(i, e, s)`** → 1 or 0. By type: string → a copy; float, double → `atof`;
  int, uint, int64, uint64 → `strtol` with the base from the prefix; vector → three `atof`s;
  entity → `atoi` (an `entity ` prefix is accepted); field → by name (fails if missing); function
  → by name (a two-character `N:` → null; fails if missing).
- **`parseentitydata(e, s, optional ofs)`**: skips `ofs` bytes (clamped); an empty rest → 0. Parses
  one `{ "key" "value" … }` block into `e` (a free `e` is zeroed, marked in use and passed to the
  spawn hook) and returns `ofs` + the bytes consumed, or 0. Keys: trailing spaces are trimmed;
  keys starting with `_` are ignored; `angle` becomes `angles = "0 yaw 0"`; `light` falls back to
  `light_lev`; unknown keys go to a warning hook; nested braces are skipped; no spawn functions
  are called.
- **`loadfromdata(s)`**: repeats `parseentitydata` on **new** entities until the input is used up.
- **`generateentitydata(e)`**: a savegame-style text block.

qcvm implements the field reflection builtins and the savegame-style text of `eprint` and
`coredump`, but not `getentityfieldstring`, `putentityfieldstring`, `parseentitydata`,
`loadfromdata` or `generateentitydata`: they fault as unbound builtins unless the host registers
them.

### Introspection

- **`checkbuiltin(fref)`**: true only if `fref` is a builtin record whose number maps to a real
  implementation.
- **`isfunction(name)`**, **`callfunction(a…, name)`**: see the [table](#numbered-builtins).
- **`externcall(prnum, name, a…)`**: `prnum` 0 is the main progs, −1 the current one, −2 the first
  one that has the function (with a single progs they are all the same). The arguments move two
  slots down and the function is called; if it is missing, `MissingFunc(name, a…)` is called if
  it exists, else it is a builtin error. FTE passes on at most five argument slots; qcvm passes
  them all.
- **`externvalue(prnum, name)`**: `&name` → the global's address (0 if missing); otherwise the
  global's three words; without such a global, a reference to the function of that name, or 0.
- **`externset(prnum, v, name)`**: writes by the global's type: three words for a vector, two for a
  64-bit type, one otherwise.
- **`abort(optional ret)`**: unwinds every frame to the engine's entry, which returns `ret`.
- **`traceon`, `traceoff`, `breakpoint`, `coredump`, `eprint`**: see the
  [table](#numbered-builtins).

### Time

- **`gettime(optional type)`**: 0 or unknown → the realtime at the start of the frame (from the
  host, or monotonic since the VM was created); 1 → the current realtime, rounded to milliseconds;
  5 → the client's simulation time (a host hook).
- **`calltimeofday`**: see the [table](#numbered-builtins).

### Digests and URIs

See [strings.md](strings.md). `digest_ptr(alg, ptr, len, [ofs])` hashes VM memory; an invalid range
is a builtin error.

### Host hooks

The class B builtins:

- `print` (unconditional), `dprint` (developer mode only), `error` (a trace, then fatal; FTE only
  warns in developer mode, qcvm is always fatal), `objerror` (fatal in CSQC, not in SSQC),
  `cprint` (centerprint), `localcmd`;
- the cvar family: FTE creates unknown cvars with the value `""`, qcvm leaves that to the host;
  the `cvar_type` bits; `registercvar` (see the [table](#numbered-builtins));
- `buf_cvarlist`: a wildcard match if the pattern contains `*` or `?`, else a prefix match; cvars
  matching the antipattern are excluded; the result is sorted;
- `cvars_haveunsaved`, `checkcommand`, `registercommand`;
- `checkextension`: advertise only what is implemented;
- `isdemo`, `isserver`;
- file access: `coredump`, `loadfromfile`, `buf_loadfile`, `addprogs`.

## Entity storage

As in FTE's CSQC.

- `maxedicts` defaults to **65,536**. `num_edicts` is a high-water mark: it only grows (until the
  VM restarts).
- The world (entity 0) is created at initialisation, zeroed and passed to the spawn hook. It is
  always in use and never freed: `remove(world)` prints "Unable to remove the world" and a trace,
  and does nothing. It is writable until `CSQC_WorldLoaded` returns, then read-only (toggled with
  `entityprotection`).
- A store into a read-only entity is a run warning and is skipped; into an out-of-range entity, a
  warning; into a free entity, allowed.
- **`spawn`**:
  1. scan *i* = 0 to `num_edicts` − 1 for the first slot that was never allocated, or that is free
     with freetime < 2 or now − freetime > 0.5 s;
  2. else append at `num_edicts`, unless that reaches `maxedicts` − 1;
  3. else take any free slot, regardless of age;
  4. else fail: fatal "no free edicts".

  Then **every field word is zeroed**, the slot is marked in use, and the spawn hook runs (CSQC:
  `.dimension_solid` and `.dimension_hit` = the global `dimension_default`, or 255 without it).
  "Now" is the engine's realtime (a host clock), not QuakeC's `time`; freetime is a float;
  `removeinstant` sets freetime = 0, so the slot can be reused at once. qcvm allocates every slot
  below `Limits::max_edicts`, which counts the world.
- **`remove`** refuses an entity that is already free (with a warning), a read-only entity and the
  world. CSQC's free hook zeroes only `.solid`, `.movetype`, `.modelindex`, `.think`, `.nextthink`,
  `.predraw`, `.drawmask` and `.renderflags` (and releases engine resources); in qcvm this list is
  `VmConfig::remove_clears`, defaulting to CSQC's. The slot is marked free with its freetime;
  other fields stay readable until it is reused.
- `wasfreed` means "not in use". `find` and iteration skip free entities; `edict_num` does not.
- `self` and `other` are ordinary globals, written by the host.

## Numbering by VM kind

CSQC and SSQC use the same numbers except as noted below; menu progs number compactly. qcvm has a
table for each (`Numbering::Csqc`, `Numbering::Ssqc`, `Numbering::Menu`), generated from FTE's
`fteextensions.qc` by `scripts/gen-builtin-numbers.py`.

Menu numbers of the class A and B builtins:

| Number | Builtins |
|---|---|
| 1 | `checkextension` |
| 2, 3 | `error`, `objerror` |
| 4 | `print` |
| 7 | `cprint` |
| 8–11 | `normalize`, `vlen`, `vectoyaw`, `vectoangles` |
| 12 | `random` |
| 13–16 | `localcmd`, `cvar`, `cvar_set`, `dprint` |
| 17–21 | `ftos`, `fabs`, `vtos`, `etos`, `stof` |
| 22–25 | `spawn`, `remove`, `find`, `findfloat` |
| 26, 27 | `findchain`, `findchainfloat` |
| 30–33 | `coredump`, `traceon`, `traceoff`, `eprint` |
| 34–37 | `rint`, `floor`, `ceil`, `nextent` |
| 38–41 | `sin`, `cos`, `sqrt`, `randomvec` |
| 42–46 | `registercvar`, `min`, `max`, `bound`, `pow` |
| 47 | `copyentity` |
| 52–57 | `strlen`, `strcat`, `substring`, `stov`, `strzone`, `strunzone` |
| 58, 59 | `tokenize`, `argv` |
| 60 | `isserver` |
| 67 | `gettime` |
| 68, 69 | `loadfromdata`, `loadfromfile` |
| 70 | `mod` |
| 71 | `cvar_string` |
| 72 | `crash` |
| 73 | `stackdump` |
| 78 and 223 | `chr2str` |
| 79 | `etof` |
| 80 | `ftoe` |
| 81 | `validstring` |
| 82–85 | `altstr_*` |
| 87, 88 | `findflags`, `findchainflags` |
| 89 | `cvar_defstring` |
| **440–449** | `buf_*` |
| 512 | `num_for_edict` |
| 532 | `log` |

SSQC differs from CSQC in:

- 532: `log` in CSQC, but `precache_vwep_model` in SSQC;
- 232–234: `clientstat`, `globalstat`, `isbackbuffered` (234 is `rotatevectorsbytag` in CSQC);
- 440: `clientcommand`;
- 501: `WritePicture` (CSQC has `ReadPicture`).

## FTE quirks

qcvm keeps FTE's deliberate design choices and fixes clear bugs; [deviations.md](deviations.md)
is the complete list. For the builtins in this document:

| Quirk | qcvm |
|---|---|
| `entityprotection` returns the new value | fixed: the previous value |
| `hash_getcb` is a no-op | fixed: calls the callback |
| an invalid string buffer handle leaves the return value untouched | fixed: 0 or null |
| `registercvar` with two arguments ignores the default | fixed: passes it on |
| `changepitch` ignores its argument | fixed: acts on it |
| `strncmp` ignores `s2ofs`; `stoi` is decimal only; an empty `tokenizebyseparator` separator hangs | fixed (see [strings.md](strings.md#fte-quirks)) |
| `vectoyaw` truncates, `vectoangles` does not | kept |
| `anglemod` loops, `changeyaw` quantises to 16 bits | kept (`anglemod` computed with a remainder) |
| `find` with `""` matches null fields, `findchain` does not | kept |
| `findfloat` compares raw bits, `findchainfloat` float values | kept |
| `buf_sort` compacts holes | kept |
| `random` never returns 0 or 1 | kept |
| `memgetval`/`memsetval` count words, `memptradd` bytes | kept |
| `objerror` is fatal in CSQC | kept |
| `log` defaults to the natural logarithm | kept |
| `ftos` prints float noise (`0.1` → `"0.100000001"`) | kept |
| replacing a hash entry removes only the newest one for the key | kept |
| `precache_file` is an engine builtin | not provided |
