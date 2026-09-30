<!-- SPDX-License-Identifier: MIT OR Apache-2.0 -->

# Deliberate deviations from FTE

Policy: qcvm reproduces FTE's *observable* behaviour (reference build: x86-64, glibc) so that
QuakeC written against FTE behaves identically — **except** where FTE has a clear bug, undefined
behaviour, a hang, or a buffer overflow. Those are fixed and listed here, with FTE's behaviour for
reference. FTE's deliberate design choices are kept even where other engines differ; they are
listed at the end so nobody "fixes" them by accident.

Some VM-level fixes can be switched back to FTE's behaviour through `FteCompat` flags in the VM
configuration. They exist so the differential test-suite can compare against FTE bit for bit;
production code should leave them off.

## VM / opcodes

| Area | FTE | qcvm | `FteCompat` flag |
|---|---|---|---|
| `NE_S` result | stores the raw `strcmp` result as a float (may be negative or >1) | stores `1.0`/`0.0` | `ne_s_raw_strcmp` |
| `LOAD_I64` on an invalid entity | zeroes three result words (one too many) | zeroes the two result words | `load_i64_zero3` |
| `SWITCH`/`CASE` state | kept in interpreter locals, reset to (float, global 0) after every call/return | kept per call frame | `switch_reset_on_call` |
| Function index bound | accepts `index == numfunctions` (off by one) | rejects `index >= numfunctions` | — |
| Pointer bound check | `p + n < used`, 32-bit arithmetic (wraps) | `p + n <= end` of the region, without wrapping | — |
| `LOADP_ITOF`/`LOADP_FTOI` | checks 1 byte, reads 4 | checks 4 bytes | — |
| 12-byte temp-string fallback read | faults | zero-fills like smaller reads | — |
| `STOREP_V`/`LOADP_V` with overlapping source and destination | depends on how the C compiler copies the vector: copying the first three words of `[1,2,3,4]` one word up leaves `[1,2,2]` in the destination with the reference build | copies as if through a temporary: `[1,2,3]` | — |
| Operand sanitizer | treats `SWITCH_*.b` as a global operand (large switches get poisoned) | `SWITCH_*.b` is a jump offset | — |
| `DIV_I64`/`DIV_U64` | unguarded (host process traps on /0 and `MIN / -1`) | `/0 → 0`, `MIN / -1 → MIN` | — |
| Entity limit | never allocates the last slot below `maxedicts` | allocates every slot below `Limits::max_edicts` (which counts the world) | — |
| `FETCH_GBL_*` with `a == 0` | reads the prefix word at index −1 | faults with an array-index error | — |
| Bitfield ops with `w == 0` or `w + p > 32` | C undefined behaviour | defined via wrapping shifts and masks | — |
| `CSTATE`/`CWSTATE` think function | stored without the progs byte | keeps the progs byte (multiprogs-safe) | — |
| `STOREP_*` into a read-only entity via a stale pointer | allowed | warns and skips the store | — |
| Resumed `sleep`/`fork` threads | restore `self`/`other` by entity number | validate against a spawn serial; a reused slot resumes as world | — |
| Runaway budget | a fresh budget for every nested execution (a builtin calling back into QuakeC starts over) | one budget per host call, shared by the calls builtins make back into QuakeC; optionally a wall-clock deadline | — |

## Builtins

| Builtin | FTE | qcvm |
|---|---|---|
| `strncmp` (and the `strcmp` alias) with `s2ofs` | range-checks `s2ofs` but never applies it | applies `s2ofs` |
| `strcasecmp`/`strncasecmp` | folds to upper case and compares signed chars (`("_","a")` → +1) | C semantics: folds to lower case, compares unsigned bytes |
| `entityprotection` | returns the *new* value | returns the *previous* value, as documented |
| `hash_getcb` | no-op stub | calls the callback once per entry (entries snapshotted first), as documented |
| `registercvar` with two arguments | ignores the default value | passes the default value to the host |
| `changepitch(ent)` | ignores `ent`, acts on `self` | acts on `ent` |
| `stoi` | decimal only (C `atoi`) | base 8/10/16 from the prefix, as documented |
| `strtoupper`/`strtolower` (Quake charset) | turn byte `0x0B` into `?` | leave it unchanged |
| `infoadd` rejected for size | the old pair has already been removed | the info string is returned unchanged |
| Invalid `strbuf` handle in `buf_getsize`/`buf_implode`/`bufstr_add` | leaves the return slot untouched | returns 0 / null |
| `tokenizebyseparator` with an empty separator | loops forever | ignores empty separators |
| `tokenize` unterminated / doubled single quotes | reads past the end of the string | the end of the string terminates; `''` is a literal quote |
| `^U` markup in `strdecolorize` | consumes 6 bytes without checking for hex digits | requires four hex digits, otherwise literal |
| Quake-charset encoder | cannot encode U+E00B (`\v`) | round-trips `\v` |
| Fixed-size output buffers (`strtoupper`, `vtos`, …) | overflow on huge inputs | produce the full string, subject to the documented length caps |
| `#0:gettime` | does not resolve in CSQC (engine name is `gettimef`) | resolves (alias) |
| `strpad` with a negative pad shorter than the string | returns 4095 spaces (signed/unsigned mix-up) | returns the string unpadded |
| `altstr_get`/`altstr_set` | count an escaped quote as a separator; `altstr_set` drops backslashes | honour escapes |
| `chr2str`/`%c` with a code ≥ 2³¹ (UTF-8) | the encoder loops forever | encodes U+FFFD |
| `\` at the very end of a `\"…` token | reads past the end of the string | ends the token |
| 6-byte UTF-8 sequences | checks only four continuation bytes | requires all five |
| Nested `` ^`u8: `` / `` =`k8: `` markup in `strdecolorize` | unbounded recursion | at most 16 levels |
| `%I` in `sprintf` | passed through to the C library | a format error, like every other unknown conversion |
| `bitshift` by 32 or more | count masked to 5 bits | everything is shifted out |
| `memgetval`/`memsetval` | compute `ptr + ofs * 4` in floating point (wrong above 2²⁴) | exact integer arithmetic |
| `memsetval` into a protected entity / at address 0 | writes | warns and skips / null-pointer error |
| `memfree` of a pointer that is not a block start | ignored silently | warns |
| `memcmp` offsets | the implementation swaps the two offset arguments | the 4th argument offsets the first pointer, as documented |
| Reads from `createbuffer` buffers | cannot reach the last byte | can |
| `json_parse` | a lenient parser (comments, trailing commas, unquoted words as numbers, case-insensitive literals, keys verbatim) that decodes `\uXXXX` from its own output buffer, with unbounded nesting | strict JSON (RFC 8259, parsed by `serde_json`): keys and strings unescaped, at most 128 levels, a leading UTF-8 byte-order mark skipped; anything else parses to null |
| `buf_loadfile` | splits lines longer than 8191 bytes | keeps lines whole |
| `bufstr_set`/`bufstr_add` beyond `Limits::string_buffer_entries` | accepts index 1,048,576 | refuses it with a warning (−1 from `bufstr_add`) |
| Hash tables, string buffers and token lists beyond `Limits::container_bytes` (64 MiB) | unbounded | `hash_createtab` returns 0, `hash_add` and `bufstr_set` do nothing, `bufstr_add` returns −1, the tokenizers stop at the budget, each with a warning |
| `sleep`/`fork` beyond `Limits::thread_bytes` (16 MiB of snapshots) | unbounded | an out-of-memory error |
| `externcall` | passes on at most five argument slots | passes every remaining argument |
| `findradius_list` | measures a different distance than `findradius` | the same test as `findradius` (distance to the box centre) |
| `error` in developer mode | a warning | always fatal (`ErrorKind::QcError`) |

## Kept on purpose (FTE design choices)

- `ftos` prints the exact float value (`0.1` → `"0.100000001"`).
- `random()` returns values strictly inside (0, 1); `RANDV*` opcodes use the inclusive range.
- `vectoyaw` truncates to whole degrees; `vectoangles` does not.
- `sprintf`: `%d`/`%x`/`*` default to *float* arguments; output stops at the first format error.
- Float→int conversion follows x86 (NaN / out of range → `INT_MIN`), and shift counts are masked
  like x86 — on every platform, for determinism.
- `IF_S` tests for null only; `NOT_S` tests for null *or* empty; `EQ_S` treats null as `""`.
- `ADD_SF` on a temp-string handle changes the slot index.
- `find` with `""` matches null fields, `findchain` does not; `findfloat` compares raw bits,
  `findchainfloat` compares float values.
- `hash_add` with replace removes only the newest entry for the key; `hash_getkey` order is
  unspecified.
- `buf_sort` compacts holes before sorting.
- `memgetval`/`memsetval` offsets count 4-byte words; `memptradd` counts bytes.
- `uri_escape` keeps `~`; the CRC16 `digest_hex` output is little-endian.
- `strdecolorize` strips every ezQuake `&r`.
- `objerror` is fatal in CSQC.
- Freed entities stay readable and writable until the slot is reused.
- `PARM`/`RETURN` globals are not preserved across nested calls from builtins.
