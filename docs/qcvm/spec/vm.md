<!-- SPDX-License-Identifier: MIT OR Apache-2.0 -->

# FTE QuakeC VM behaviour: opcodes, memory, loader

This document describes FTEQW behaviour, written from scratch in our own words after studying FTE
as a behavioural specification. No FTE (GPL) code is reproduced here; `file:line` references into
`fteqw/engine/` are pointers for behaviour lookup only.

Where this crate deliberately deviates from FTE, see [deviations.md](deviations.md); the
deviations are also noted where they apply below.

**Notation.** `a`, `b` and `c` are a statement's operands, usually global indices; `A`, `B` and
`C` are the values in those globals. `.f`, `.i` and `.u` read a value as a float, an int or an
unsigned int, and `[k]` is component *k* of a vector. `trunc` is C's float-to-int truncation.

## Overview

- Opcodes 0–281 are numbered as in QCVMv7.md (checked against the enum in `qclib/pr_comp.h`).
  `OP_NUMREALOPS` is 282; 283–416 exist only in the compiler. FTE never executes opcodes ≥ 282:
  204 (`GADDRESS`) is an explicit error, and 212, 214 and ≥ 282 are "Bad opcode".
- The breakpoint bit 0x8000 is masked off, the debug hook runs, and the opcode is dispatched again.
- KTX's `csprogs.dat` is version 7 with secondary version 0x021B1461 (FTE, 16-bit records), CRC
  22390 (FTE's CSQC CRC), `blockscompressed` 0 and `ofslinenums` 0, so FTE falls back to
  `csprogs.lno`. `numbodylessfuncs` is 0 but its offset is not: check counts, not offsets.
- The string table starts with two NULs: offset 0 is null, offset 1 the canonical non-null `""`.

## Opcode names

FTE's names without the `OP_` prefix; the row gives the tens, the column the units.

| | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 |
|---|---|---|---|---|---|---|---|---|---|---|
| **0** | `DONE` | `MUL_F` | `MUL_V` | `MUL_FV` | `MUL_VF` | `DIV_F` | `ADD_F` | `ADD_V` | `SUB_F` | `SUB_V` |
| **10** | `EQ_F` | `EQ_V` | `EQ_S` | `EQ_E` | `EQ_FNC` | `NE_F` | `NE_V` | `NE_S` | `NE_E` | `NE_FNC` |
| **20** | `LE_F` | `GE_F` | `LT_F` | `GT_F` | `LOAD_F` | `LOAD_V` | `LOAD_S` | `LOAD_ENT` | `LOAD_FLD` | `LOAD_FNC` |
| **30** | `ADDRESS` | `STORE_F` | `STORE_V` | `STORE_S` | `STORE_ENT` | `STORE_FLD` | `STORE_FNC` | `STOREP_F` | `STOREP_V` | `STOREP_S` |
| **40** | `STOREP_ENT` | `STOREP_FLD` | `STOREP_FNC` | `RETURN` | `NOT_F` | `NOT_V` | `NOT_S` | `NOT_ENT` | `NOT_FNC` | `IF_I` |
| **50** | `IFNOT_I` | `CALL0` | `CALL1` | `CALL2` | `CALL3` | `CALL4` | `CALL5` | `CALL6` | `CALL7` | `CALL8` |
| **60** | `STATE` | `GOTO` | `AND_F` | `OR_F` | `BITAND_F` | `BITOR_F` | `MULSTORE_F` | `MULSTORE_VF` | `MULSTOREP_F` | `MULSTOREP_VF` |
| **70** | `DIVSTORE_F` | `DIVSTOREP_F` | `ADDSTORE_F` | `ADDSTORE_V` | `ADDSTOREP_F` | `ADDSTOREP_V` | `SUBSTORE_F` | `SUBSTORE_V` | `SUBSTOREP_F` | `SUBSTOREP_V` |
| **80** | `FETCH_GBL_F` | `FETCH_GBL_V` | `FETCH_GBL_S` | `FETCH_GBL_E` | `FETCH_GBL_FNC` | `CSTATE` | `CWSTATE` | `THINKTIME` | `BITSETSTORE_F` | `BITSETSTOREP_F` |
| **90** | `BITCLRSTORE_F` | `BITCLRSTOREP_F` | `RAND0` | `RAND1` | `RAND2` | `RANDV0` | `RANDV1` | `RANDV2` | `SWITCH_F` | `SWITCH_V` |
| **100** | `SWITCH_S` | `SWITCH_E` | `SWITCH_FNC` | `CASE` | `CASERANGE` | `CALL1H` | `CALL2H` | `CALL3H` | `CALL4H` | `CALL5H` |
| **110** | `CALL6H` | `CALL7H` | `CALL8H` | `STORE_I` | `STORE_IF` | `STORE_FI` | `ADD_I` | `ADD_FI` | `ADD_IF` | `SUB_I` |
| **120** | `SUB_FI` | `SUB_IF` | `CONV_ITOF` | `CONV_FTOI` | `LOADP_ITOF` | `LOADP_FTOI` | `LOAD_I` | `STOREP_I` | `STOREP_IF` | `STOREP_FI` |
| **130** | `BITAND_I` | `BITOR_I` | `MUL_I` | `DIV_I` | `EQ_I` | `NE_I` | `IFNOT_S` | `IF_S` | `NOT_I` | `DIV_VF` |
| **140** | `BITXOR_I` | `RSHIFT_I` | `LSHIFT_I` | `GLOBALADDRESS` | `ADD_PIW` | `LOADA_F` | `LOADA_V` | `LOADA_S` | `LOADA_ENT` | `LOADA_FLD` |
| **150** | `LOADA_FNC` | `LOADA_I` | `STORE_P` | `LOAD_P` | `LOADP_F` | `LOADP_V` | `LOADP_S` | `LOADP_ENT` | `LOADP_FLD` | `LOADP_FNC` |
| **160** | `LOADP_I` | `LE_I` | `GE_I` | `LT_I` | `GT_I` | `LE_IF` | `GE_IF` | `LT_IF` | `GT_IF` | `LE_FI` |
| **170** | `GE_FI` | `LT_FI` | `GT_FI` | `EQ_IF` | `EQ_FI` | `ADD_SF` | `SUB_S` | `STOREP_C` | `LOADP_C` | `MUL_IF` |
| **180** | `MUL_FI` | `MUL_VI` | `MUL_IV` | `DIV_IF` | `DIV_FI` | `BITAND_IF` | `BITOR_IF` | `BITAND_FI` | `BITOR_FI` | `AND_I` |
| **190** | `OR_I` | `AND_IF` | `OR_IF` | `AND_FI` | `OR_FI` | `NE_IF` | `NE_FI` | `GSTOREP_I` | `GSTOREP_F` | `GSTOREP_ENT` |
| **200** | `GSTOREP_FLD` | `GSTOREP_S` | `GSTOREP_FNC` | `GSTOREP_V` | `GADDRESS` | `GLOAD_I` | `GLOAD_F` | `GLOAD_FLD` | `GLOAD_ENT` | `GLOAD_S` |
| **210** | `GLOAD_FNC` | `BOUNDCHECK` | `UNUSED` | `PUSH` | `POP` | `SWITCH_I` | `GLOAD_V` | `IF_F` | `IFNOT_F` | `STOREF_V` |
| **220** | `STOREF_F` | `STOREF_S` | `STOREF_I` | `STOREP_I8` | `LOADP_U8` | `LE_U` | `LT_U` | `DIV_U` | `RSHIFT_U` | `ADD_I64` |
| **230** | `SUB_I64` | `MUL_I64` | `DIV_I64` | `BITAND_I64` | `BITOR_I64` | `BITXOR_I64` | `LSHIFT_I64I` | `RSHIFT_I64I` | `LE_I64` | `LT_I64` |
| **240** | `EQ_I64` | `NE_I64` | `LE_U64` | `LT_U64` | `DIV_U64` | `RSHIFT_U64I` | `STORE_I64` | `STOREP_I64` | `STOREF_I64` | `LOAD_I64` |
| **250** | `LOADA_I64` | `LOADP_I64` | `CONV_UI64` | `CONV_II64` | `CONV_I64I` | `CONV_FD` | `CONV_DF` | `CONV_I64F` | `CONV_FI64` | `CONV_I64D` |
| **260** | `CONV_DI64` | `ADD_D` | `SUB_D` | `MUL_D` | `DIV_D` | `LE_D` | `LT_D` | `EQ_D` | `NE_D` | `STOREP_I16` |
| **270** | `LOADP_I16` | `LOADP_U16` | `LOADP_I8` | `BITEXTEND_I` | `BITEXTEND_U` | `BITCOPY_I` | `CONV_UF` | `CONV_FU` | `CONV_U64D` | `CONV_DU64` |
| **280** | `CONV_U64F` | `CONV_FU64` | | | | | | | | |

## Semantics

From `execloop.h`.

### Conventions

- `a`, `b` and `c` are global indices; in 16-bit progs they are *unsigned* 16-bit, so up to 65,535
  globals are addressable.
- These operands are raw immediates instead:
  - jump offsets: `GOTO.a`; `b` of `IF_I`, `IFNOT_I`, `IF_F`, `IFNOT_F`, `IF_S`, `IFNOT_S`,
    `SWITCH_*` and `CASE`; `CASERANGE.c`;
  - the bounds of `BOUNDCHECK`: `b` is the exclusive upper bound and `c` the inclusive lower bound,
    both unsigned;
  - `a` of `LOADA_*` and `FETCH_GBL_*` is the array's base global, and `a` of `GLOBALADDRESS` a
    global number.
- Jump offsets are sign-extended from 16 bits in 16-bit progs and are 32-bit in 32-bit progs. The
  loop pre-increments: a jump by *k* at statement *s* lands on *s* + *k*, so *k* = 0 loops until
  the runaway counter stops it.
- v6 comparisons and `NOT_*` write the float 1.0 or 0.0; all int, uint, mixed, i64 and double
  comparisons write the **int** 0 or 1.
- Multi-word writes go component by component (0, 1, 2), and scalar operands are read once where
  noted; this order decides the result when operands overlap.
- Float-to-int conversion is C truncation; on x86 values out of range and NaN give `INT_MIN`
  (`i64::MIN` for 64-bit results). Rust's `as` saturates, so qcvm emulates x86.
- Shift counts that are negative or at least the width are masked to 5 bits (6 for 64-bit), as x86
  does.

### Floats and vectors

- `MUL_F`, `ADD_F`, `SUB_F`, `DIV_F`: IEEE; no guard against division by zero.
- `MUL_V`: the dot product, summed in x, y, z order.
- `MUL_FV`: reads `A.f` once; `C[k] = A · B[k]`.
- `MUL_VF`: reads `B.f` once; `C[k] = A[k] · B`.
- `ADD_V`, `SUB_V`: per component.
- `DIV_VF` (139): reads `B` once; `C[k] = A[k] / B`.

### v6 comparisons and logic

These write floats.

- `EQ_F`, `NE_F`, `LE_F`, `GE_F`, `LT_F`, `GT_F`: IEEE (−0 == 0, NaN is unequal to everything).
- `EQ_V`: all components equal; `NE_V`: any component differs.
- `EQ_E`, `NE_E`, `EQ_FNC`, `NE_FNC`: compare all 32 bits.
- `EQ_S`, `NE_S`:
  1. identical references are equal;
  2. if exactly one side is null (0), they are equal iff the other resolves to an empty string;
  3. otherwise the bytes are compared.

  FTE's `NE_S` stores the raw `strcmp` result as a float; qcvm stores 1.0.
- `NOT_F`, `AND_F`, `OR_F` use **masked float truth**: true iff the low 31 bits are non-zero (−0
  is false, denormals are true).
- `NOT_V`: all three components == 0.0. `NOT_S`: null **or** empty. `NOT_ENT`: `A.i == 0`.
  `NOT_FNC`: the low 24 bits are 0.
- `BITAND_F`, `BITOR_F`: `float(trunc(A) op trunc(B))`.

### Branches

Each branch decrements the runaway counter, taken or not.

- `IF_I`, `IFNOT_I`: raw 32-bit truth (−0.0 is true).
- `IF_F`, `IFNOT_F`: masked float truth.
- `IF_S`, `IFNOT_S`: true iff the reference is non-zero — **null only**; a non-null `""` is true.
- `GOTO`: unconditional.

### Global stores

- `STORE_F`, `STORE_S`, `STORE_ENT`, `STORE_FLD`, `STORE_FNC`, `STORE_I`, `STORE_P` (31, 33–36,
  113, 152): `B = A`, one word.
- `STORE_V`: three words.
- `STORE_I64` (246): `B = A`, **two words** (`c` is unused, 0). csprogs uses it for the two-word
  tails of struct copies.
- `STORE_IF`: `B.f = (float)A.i`. `STORE_FI`: `B.i = trunc(A.f)`.

### Entity fields

The field offset is *f* = `B.i` (plus `fieldadjust`, which is 0); *fieldsize* is the number of
bytes per entity. An entity is invalid if `A.i`, unsigned, is ≥ `num_edicts`.

| Opcode | Invalid entity | Field check | Action |
|---|---|---|---|
| `LOAD_F/S/ENT/FLD/FNC/I/P` | warn, `C.i = 0` | (*f* + 1) · 4 > *fieldsize* → warn, `C.i = 0` | one word |
| `LOAD_V` | warn, `C[0]`–`C[2]` = 0 | (*f* + 3) · 4 > *fieldsize* → warn, only `C[0] = 0` | three words |
| `LOAD_I64` | warn, `C[0]`–`C[2]` = 0 (one word too many; qcvm zeroes two) | (*f* + 2) · 4 > *fieldsize* → warn, `C[0] = 0` | two words |
| `ADDRESS` | warn, `C` unchanged | none | `C` = the field's byte address; **a read-only entity → warn, `C = 0xFFFFFFFF`** |
| `STOREF_F/S/I` | warn, skip | *f* · 4 ≥ *fieldsize* → warn, skip | field = `C`; a read-only entity → warn, skip |
| `STOREF_V/I64` | the same | the first word only | three / two words from `C` |

- Freed entities are not rejected: reads see stale data and writes succeed.
- `num_edicts` is refreshed after every builtin.
- "Warn" means a message and a stack trace to the host; execution continues.

### Pointer loads and stores

*used* is the number of addressable bytes in use.

- A read of *n* bytes at *p* is valid if *p* + *n* < *used*. *p* = 0 is allowed: it reads real
  memory at address 0 (qcvm may return memory there too).
- A write is valid if 1 ≤ *p* and *p* + *n* < *used*.
- Otherwise:
  1. If the base operand is a **temp-string handle**, the access is redone inside that temp string
     at the byte offset. Reads past its end return 0 (for accesses of up to 8 bytes; a 12-byte
     vector read fails); writes past its end grow it, zero-filled, up to 1 MiB.
  2. If the computed address is 0xFFFFFFFF, reads give 0 in every destination word and writes are
     skipped. This pairs with `ADDRESS` of a read-only entity.
  3. Otherwise the access faults with "bad pointer read/write" (a write to null has its own
     message).

qcvm checks *p* + *n* ≤ the end of the region, without 32-bit wrap-around, and zero-fills 12-byte
temp-string reads too.

| Opcode | Address | Size | Value |
|---|---|---|---|
| `STOREP_F/S/ENT/FLD/FNC/I` | `B + C.i · 4` | 4 | `A` |
| `STOREP_V` | `B + C.i · 4` | 12 | `A[0]`–`A[2]` |
| `STOREP_I64` | `B + C.i · 4` | 8 | `A`, two words |
| `STOREP_IF`, `STOREP_FI` | `B + C.i · 4` | 4 | `(float)A.i`, `trunc(A.f)` |
| `STOREP_C` (177) | `B + C.i` (bytes, `C` an int) | 1 | the low 8 bits of `trunc(A.f)` |
| `STOREP_I8` (223) | `B + C.i` | 1 | the low 8 bits of `A.i` |
| `STOREP_I16` (269) | `B + C.i · 2` | 2 | the low 16 bits of `A.i` |
| `LOADP_F/S/ENT/FLD/FNC/I` | `A + B.i · 4` | 4 | `C` = the word |
| `LOADP_V` | `A + B.i · 4` | 12 | `C[0]`–`C[2]` |
| `LOADP_I64` | `A + B.i · 4` | 8 | `C`, two words |
| `LOADP_C` (178) | `A + trunc(B.f)` — **a float index** | 1 | `C.f` = the unsigned byte |
| `LOADP_U8`, `LOADP_I8` | `A + B.i` | 1 | `C.i`, zero- / sign-extended |
| `LOADP_U16`, `LOADP_I16` | `A + B.i · 2` | 2 | `C.i`, zero- / sign-extended |
| `LOADP_ITOF`, `LOADP_FTOI` | **`A` only** (`B` is ignored) | checked as 1 byte, reads 4 (qcvm checks 4) | `C.f = (float)int`, `C.i = trunc(f)`; no temp-string or −1 fallback |

- The `STOREP` index `c` is a *global reference*: v6 code has `c` = 0 and so reads global 0's
  value. **Global 0 must stay 0.**
- When the source and destination of `STOREP_V` or `LOADP_V` overlap, FTE's result depends on how
  its C compiler copies the vector; qcvm copies as if through a temporary.

### Compound stores (Hexen 2)

The non-pointer forms update `B` in place and **do not write `C`**:

- `MULSTORE_F`: `B *= A`.
- `MULSTORE_VF`: reads `A.f` once, then `B[k] *= A`.
- `DIVSTORE_F`, `ADDSTORE_F`, `ADDSTORE_V`, `SUBSTORE_F`, `SUBSTORE_V`: likewise.
- `BITSETSTORE_F`: `B = float(trunc(B) | trunc(A))`.
- `BITCLRSTORE_F`: `B = float(trunc(B) & ~trunc(A))`.

In the pointer forms `B` is a raw byte pointer, without an index. A failed write check (4 or 12
bytes) is a fatal run error, with no temp-string or −1 fallback.

- `MULSTOREP_F`, `DIVSTOREP_F`, `ADDSTOREP_F`, `SUBSTOREP_F`: `*B op= A`, and **`C.f` = the new
  value**.
- `MULSTOREP_VF`: reads `A.f` once; for each *k*, `*B[k] *= A` and `C[k]` = the new value.
- `ADDSTOREP_V`, `SUBSTOREP_V`: interleaved per component, `*B[k] op= A[k]; C[k] = *B[k]` for
  *k* = 0, 1, 2. csprogs emits these with `c == a`, so the interleaving matters.
- `BITSETSTOREP_F`, `BITCLRSTOREP_F`: update `*B`; `C` is not written.

### `FETCH_GBL_*` (80–84)

*i* = `trunc(B.f)`. The prefix `global[a − 1].i` holds the element count − 1 (inclusive; vector
arrays count vectors), and (unsigned) *i* > (unsigned) prefix is fatal: "array index out of
bounds".

- `FETCH_GBL_F/S/E/FNC`: `C = global[a + i]`.
- `FETCH_GBL_V`: `C` = the three words at `global[a + 3i]`.

With `a` = 0 FTE reads the prefix at index −1; qcvm faults with an array-index error.

### `RAND*` (92–97)

*r* is a 15-bit random number (0–32767).

- `RAND0`: `C.f = r / 32768`. `RAND1`: `(r / 32768) · A`. `RAND2`: `A + (r / 32768) · (B − A)`.
- `RANDV0`, `RANDV1`, `RANDV2`: the same for each of x, y, z, but with *r* / **32767** (the
  inclusive range [0, 1]).
- These exist for Hexen 2 targets. If the loader detects Hexen 2 calling, `RAND*` statements with
  `c` = 0 get `c` = 1 (`RETURN`).

### `SWITCH`, `CASE`

- `SWITCH_F/V/S/E/FNC` (98–102) and `SWITCH_I` (215) record a *live reference* to global `a` and a
  kind, then jump by `b`.
- `CASE`: if the value matches `A`, jump by `b`.
- `CASERANGE`: if `A` ≤ value ≤ `B` (inclusive), jump by `c`.

| Kind | `CASE` | `CASERANGE` |
|---|---|---|
| F | float == (−0 == 0) | inclusive |
| V | all three components == | a per-component box |
| S | **content** comparison (null ≡ `""`) | fatal |
| E, FNC, I | 32-bit int == | signed, inclusive |

- `SWITCH` decrements the runaway counter; `CASE` and `CASERANGE` only when taken.
- FTE keeps the switch state in interpreter locals, so it resets to (float, global 0) on re-entry
  after a call or return. qcvm keeps it per frame, a superset.

### Calls (51–59, 105–112)

1. `CALLnH`: `CALL2H`–`CALL8H` first copy three words `C` → `PARM1` (globals 7–9), then every H
   form copies three words `B` → `PARM0` (4–6). The order matters when they overlap.
2. argc = *n*.
3. The progs number is `A >> 24`, the function index `A & 0xFFFFFF`.
4. Index 0, an index beyond the function table, or a bad progs number fault with "NULL function"
   or "invalid function". FTE accepts index == `numfunctions` (off by one); qcvm rejects every
   index ≥ `numfunctions`.
5. If `first_statement` ≤ 0, the function is builtin *N* = −`first_statement`. A missing builtin is
   fatal ("Builtin N:name not implemented"). After the builtin, `num_edicts` is refreshed and
   execution continues.
6. Otherwise the QuakeC function is entered.

Every call decrements the runaway counter.

### `RETURN`, `DONE`

Decrement the runaway counter; copy the three words at global `a` to globals 1–3, word by word
(`a` = 0 zeroes `RETURN`); pop the frame; stop when the depth is back at the entry depth.

### Animation opcodes

CSQC behaviour, `client/pr_csqc.c:7868-7967`. There are no read-only checks.

- `STATE` (60) calls the state op with (`A.f`, `B`): `self.nextthink = time + 0.1`,
  `self.think = B`, `self.frame = A.f`. Hexen 2 SSQC uses 0.05.
- `CSTATE` (85), with *first* = `A.f`, *last* = `B.f` and *fn* the current function's index
  without the progs byte:
  1. `self.nextthink = time + 0.1`, `self.think = fn`;
  2. if a float global `cycle_wrapped` exists, set it to 0;
  3. if *first* > *last*, go backwards (the range [*last*, *first*], step −1), else forwards
     ([*first*, *last*], step +1);
  4. if `self.frame` is outside the range, set it to *first*; otherwise add the step, and if that
     leaves the range, set it to *first* and `cycle_wrapped` to 1.
- `CWSTATE` (86): the same on `self.weaponframe`.
- `THINKTIME` (87): entity `A` (invalid → the world) gets `nextthink = time + B.f`.

qcvm keeps the progs byte in the think function, which matters with multiprogs.

### Integers and mixed types

- `ADD_I`, `SUB_I`, `MUL_I` wrap.
- `DIV_I`: `B` = 0 → 0; `INT_MIN / −1` → `INT_MAX`; otherwise truncating division. There is no
  modulo opcode.
- `BITAND_I`, `BITOR_I`, `BITXOR_I`; `RSHIFT_I` is arithmetic; `LSHIFT_I` (shift counts as above).
- `EQ_I`, `NE_I`, `LE_I`, `GE_I`, `LT_I`, `GT_I`, `NOT_I` → int.
- `ADD_FI`, `ADD_IF`, `SUB_FI`, `SUB_IF`, `MUL_IF`, `MUL_FI`, `DIV_IF`, `DIV_FI` → float, with
  the int converted.
- `MUL_VI`, `MUL_IV` → a float vector, with the int read once.
- `LE_IF`, `GE_IF`, `LT_IF`, `GT_IF`, `LE_FI`, `GE_FI`, `LT_FI`, `GT_FI`, `EQ_IF`, `EQ_FI`,
  `NE_IF`, `NE_FI` (165–174, 195, 196) → int 0/1, compared as floats.
- `BITAND_IF`, `BITOR_IF`, `BITAND_FI`, `BITOR_FI` → **int**, with the float side truncated.
- `AND_I`, `OR_I`, `AND_IF`, `OR_IF`, `AND_FI`, `OR_FI` → int; a float side is true iff ≠ 0.0
  (unmasked: −0 is false, NaN true).
- `CONV_ITOF` rounds to nearest; `CONV_FTOI` truncates.

### Pointers, strings, indexed globals

- `GLOBALADDRESS`: `C.i = globals_base + 4 · (a + B.i)`; `b` = 0 relies on global 0 being 0.
- `ADD_PIW`: `C = A + 4 · B.i`.
- `ADD_SF`: `C.i = A.i + trunc(B.f)`, a plain integer add; on a temp-string handle it changes the
  slot index (an FTE quirk, reproduced).
- `SUB_S`: `C.i = A.i − B.i`.
- `LOADA_F/S/ENT/FLD/FNC/I`: *i* = `a + B.i`, valid if 0 ≤ *i* < `numglobals`, else the fault
  "bad array read". `LOADA_V` is valid if *i* + 2 < `numglobals`, `LOADA_I64` if *i* + 1 <
  `numglobals`.
- `GLOAD_I/F/FLD/ENT/S/FNC`: `C = global[A.i]`, fatal unless 0 ≤ `A.i` < `numglobals`; `GLOAD_V`
  is bounded by `numglobals` − 2.
- `GSTOREP_*`: `global[B.i] = A`, with the same bounds; `GSTOREP_V` writes three words.
- `GADDRESS`: always fatal.
- `BOUNDCHECK`: passes iff `c` ≤ `A.i` < `b`, compared **unsigned** (negative values fail);
  otherwise a fault.
- `UNUSED` (212), `POP` (214): fatal "Bad opcode".
- `PUSH` (213): `C.i` = the byte address of the local-stack word at *localstack_used* +
  *pushed_so_far*; then *pushed_so_far* += `A.u` (`A` is a global holding a word count). It is
  fatal ("pushed too much") if the total reaches the local-stack size. Pushed space is not zeroed,
  is word-aligned and is released when the function returns.

### Unsigned, 64-bit, double

- `LE_U`, `LT_U`: unsigned. `DIV_U`: `B` = 0 → 0. `RSHIFT_U`: logical. `CONV_UF`; `CONV_FU`
  truncates.
- 64-bit integers are two words, the low word first. `ADD_I64`, `SUB_I64`, `MUL_I64` wrap.
- **`DIV_I64` and `DIV_U64` are unguarded in FTE** (the process traps); qcvm defines division by
  zero as 0 and `MIN / −1` as `MIN` (wrapping).
- `BITAND_I64`, `BITOR_I64`, `BITXOR_I64`; `LSHIFT_I64I`, `RSHIFT_I64I` (arithmetic) and
  `RSHIFT_U64I` (logical) take the count from `B.i`.
- `LE_I64`, `LT_I64`, `EQ_I64`, `NE_I64`, `LE_U64`, `LT_U64` → int.
- `CONV_UI64` zero-extends, `CONV_II64` sign-extends, `CONV_I64I` keeps the low 32 bits.
  `CONV_I64F`, `CONV_U64F`, `CONV_I64D`, `CONV_U64D` round to nearest; `CONV_FI64`, `CONV_FU64`,
  `CONV_DI64`, `CONV_DU64` truncate.
- Doubles: `ADD_D`, `SUB_D`, `MUL_D`, `DIV_D` are IEEE; `LE_D`, `LT_D`, `EQ_D`, `NE_D` → int;
  `CONV_FD` widens, `CONV_DF` rounds.

### Bitfields

The descriptor is `B.u`: the width *w* is the low byte, the position *p* = `B.u >> 8`.

- `BITEXTEND_I`: `(A << (32 − w − p)) >> (32 − w)`, arithmetic.
- `BITEXTEND_U`: the same, logical.
- `BITCOPY_I` reads, modifies and writes `C`: with *m* = 2^*w* − 1,
  `C = (C & ~(m << p)) | ((A & m) << p)`.
- *w* = 0 and *w* + *p* > 32 are undefined behaviour in FTE; qcvm defines them through wrapping
  shifts and masks.

## Address space

`qclib/initlib.c:87-152`, `pr_edict.c:3036-3042,3670-3672`.

FTE has one flat addressable block. Pointers and non-temp string references are byte offsets from
its start. Allocations are 4-byte aligned, in this order:

1. the progs' string table, **at address 0** (`numstrings` rounded up to a multiple of 4, with at
   least one extra NUL);
2. the globals: 4 · `numglobals` bytes and **12 bytes of zero padding** (three words);
3. the local-save and `PUSH` stack: 1,048,576 words and 4 bytes;
4. other progs;
5. the world's fields;
6. the heap (`memalloc`: first fit, an 8-byte header, 64-byte rounding, returns the block + 8,
   zeroed). The field blocks of entities 1 and up live here.

In csprogs the strings take 44,112 bytes, so global *g* is at byte 44,112 + 4*g*.

Values: an entity is its number (0 is the world); a field is a word index; a function is
`index | progs << 24`; a pointer is a byte address. String references are told apart by their top
two bits:

| Top bits | Reference |
|---|---|
| `00`, `01` | a plain offset into the block (≥ *used* → `""` and a warning) |
| `10` | a temp string, `0x80000000 \| index` (the low 30 bits) |
| `11` | an engine string, `0xC0000000 \| index` into a host table |

Null is `""`. Pointer writes can corrupt anything in the block: strings, globals, saved locals,
entities.

**Read-only entities.** The CSQC world is writable during initialisation, `CSQC_Init` and
`CSQC_WorldLoaded`, and read-only once `CSQC_WorldLoaded` returns; `entityprotection(ent, 0|1)`
toggles protection. FTE enforces it only in `ADDRESS` (the −1 sentinel) and `STOREF` (the store is
skipped) — not in `STOREP` through an older pointer, in builtins or in the animation opcodes. qcvm
also warns and skips `STOREP` into a read-only entity.

## Temp strings and garbage collection

With QCGC on:

- Temp strings are allocated **outside** the block, in a slot table that starts with 1,024 slots
  and grows to 2 · max + 1,024. A cursor scans upward for a free slot, wrapping around. Sizes are
  rounded up to 4; the handle is `0x80000000 | slot`.
- `PR_TempString`, `strcat`, `strzone`, `SetStringField` and `NewString` all make temp strings.
- Collection runs only when an **outermost** execute returns (depth 0), and only if both the live
  count and the cursor are ≥ max/2. If the live count is still ≥ max/2 after the sweep, the table
  doubles.
- Roots are conservative: every aligned word in [0, *used*) whose top bits are `10` marks slot
  `word & 0x3FFFFFFF`. That covers strings, globals, the whole local stack (stale parts
  included), all entity fields (freed entities included) and all heap blocks (free ones included).
  Temps referenced only from engine memory or from other temps are collected.
- `strzone` is `strcat` (a new temp); `strunzone` does nothing.
- A freed temp reads as `""` with a warning, pointer operations on it fault, and a reused slot
  silently aliases (ABA).
- Temps are mutable: `STOREP_C` and friends write and grow them. `t[i]` works; `(t+1)[0]` reads a
  different temp.

## Execution environment

- **Limits.** Call depth 1,024 frames (overflow: trace, unwind, abort). The local-save and `PUSH`
  stack holds 1,048,576 words.
- **Runaway counter.** 100,000,000 per execute call, with a fresh budget for nested ones;
  decremented at `GOTO`, the six `IF`s, `SWITCH`, taken `CASE`/`CASERANGE`, calls and
  `RETURN`/`DONE`. qcvm gives each host call one budget, shared with the calls builtins make back
  into QuakeC.
- **Entering a function.**
  1. Push a frame: the caller's statement, function, progs number, pushed count and trace flag.
  2. Add the caller's pushed words to the stack in use.
  3. Copy the callee's globals [`parm_start`, `parm_start` + `locals`) onto the local stack.
  4. Copy each parameter slot *i* < `numparms`: `parm_size[i]` words from global 4 + 3*i* to
     consecutive locals from `parm_start`.
  5. Reset the pushed count to 0. Locals are not zeroed.

  Leaving reverses these steps.
- **Errors.** Warnings continue with a fallback; faults print a trace and abort unless a debugger
  is attached; run errors print a trace and abort. An abort in FTE's CSQC tears the VM down;
  `AbortStack` can unwind to the current entry depth, restoring locals.
- **Re-entrancy.** A nested execute saves the entry depth, sets it to the current depth, runs
  until it is back there, and restores it. **`PARM0`–`PARM7`, `RETURN` and argc are not
  preserved** across nested calls: builtins read their arguments first and write their result
  last.
- The engine saves and restores `self` itself. The predraw loop, for example, saves `self`, sets
  it, runs `.predraw`, reads the float in `RETURN` (non-zero means skip) and restores `self`.
- There is no garbage collection during nested runs.

## Builtin binding

- **`#N`**: `first_statement` = −*N*, bound at call time through the host's table. The CSQC table
  has 800 slots that default to a fatal "Builtin N:name not implemented. CSQC is not compatible."
- **`#0`**: at load time, every function record with index > 0 and `first_statement` == 0 is
  resolved by its own name through the host's `MapNamedBuiltin` (CSQC assigns free slots counting
  down from 799). Unknown names stay 0 and are fatal when called.
- **Bodyless functions** (`numbodylessfuncs`, a list of NUL-separated names) are extern QuakeC
  functions from other progs, linked into the same-named globals — **not** builtins.
- argc comes from the opcode. Builtin records have `numparms` = the declared count and every
  `parm_size` 0; both are ignored.

## Loader

The header is 60 bytes (92 for version 7); fteqcc writes a banner after it. Offsets are relative
to the start of the file. Check counts, not offsets.

| Version | Format |
|---|---|
| 6 | 16-bit records |
| 3 (QTest) | 12-byte statements (a u32 line number, then op, a, b, c), 12-byte defs (type, **name**, offset), 64-byte functions (32-bit `parm_size` fields, `parm_start` after `numparms`); converted |
| 7, secondary 0x021B1461 | FTE16: v6 record widths plus the extensions |
| 7, secondary 0x65167402 | FTE32: 16-byte statements, 12-byte defs |
| 7, secondary `"UH27"` | uHexen2: 32-bit records, opcode = raw >> 16, def type = raw >> 16, Hexen 2 calling forced; otherwise treated as v6 |
| 7, secondary `"KKQW"` | KK7: 32-bit statements and 16-bit defs; otherwise treated as v6 |
| 7, any other secondary | assumed KK7 (with a note) |
| anything else | rejected |

Function records are 36 bytes, except in QTest.

Load-time rewriting:

1. **Hexen 2 calling** (16-bit, FTE32 and KK7): if any `CALL1`–`CALL8` has `b` ≠ 0, every
   `CALL1`–`CALL8` becomes the matching `CALLnH` (+53), and `RAND*` with `c` = 0 get `c` = 1.
2. **Operand sanitizer**: a statement with any operand ≥ `numglobals` + 3 is poisoned ("Bad
   opcode" if executed). Exempt are `GOTO.a`; `b` of the six `IF`s, `BOUNDCHECK` and `CASE`; `c`
   of `BOUNDCHECK` and `CASERANGE`. FTE does not exempt `b` of `SWITCH_*`, so large switches are
   poisoned; qcvm treats it as the jump offset it is.
3. **Global fixups**: function globals get the progs number in their top byte (0 for the main
   progs); initialised **pointer**-typed globals with bit 31 set are relocations (clear the bit,
   add the byte address of the globals); field-typed globals become engine offsets (qcvm keeps the
   progs' layout); the float global `thisprogs` is set to the progs number, and
   `__ext__fasttrackarrays` to 1 if present.

Also:

- Def flags 0x8000 (save) and 0x4000 (shared) are masked off the type.
- Compressed blocks are a u32 length followed by zlib data. FTE refuses all compressed progs, and
  so does qcvm.
- The CRC is informational: CSQC accepts any (DP-compatible CRCs are 52195 and 23147).
- A `.lno` file is `"LNOF"` (0x464F4E4C, little-endian), version 1, `numglobaldefs`, `numglobals`,
  `numfielddefs`, `numstatements`, then a u32 line number per statement. It is ignored if the
  counts do not match the progs.

## Easy to miss

- Global 0 stays 0.
- The globals have a three-word zero tail, so operands up to `numglobals` + 2 are legal for
  three-word reads.
- String offset 0 is null, offset 1 the canonical `""`.
- Arguments larger than three words are split over consecutive `PARM` slots; beyond eight slots
  they go through `$parm8…` globals, which the callee's prologue copies. For struct returns larger
  than three words the caller `PUSH`es space and stores a pointer to it in `RETURN`.
- Arrays: reads use `LOADA_*`, stores `GLOBALADDRESS` and `STOREP_*`; bounds are checked with
  `BOUNDCHECK idx, size, 0`; dynamic local arrays use `PUSH`.
- Looking up an entry point by name prefers the value of a same-named function-typed global over
  the function table's name.

## Interpreter variants

- The 16- and 32-bit loops are identical except for the operand width and the sign extension of
  jumps.
- The debug loop adds profiling, watchpoints and stepping.
- `SIMPLE_QCVM`, `NOLEGACY` (freed-entity checks) and `PARANOID` (`ADDRESS` validates the field)
  are not the default. The JIT is compiled out.

## Corrections to QCVMv6.md and QCVMv7.md

Where FTE differs from those notes:

- the compound stores (see above);
- `IF_S`/`IFNOT_S` test for null only;
- out-of-range pointer reads fault, and a read from null returns memory;
- the `STOREP` index is `global[c]`;
- `LOADP_C` takes a float index; `LOADP_ITOF`/`LOADP_FTOI` ignore `B`;
- bodyless functions are not `#0` builtins;
- the compression codec is zlib, and compressed progs are refused;
- an unknown secondary version means KK7;
- the switch reference is live, `SWITCH_S` compares contents, and a string `CASERANGE` is fatal;
- `CALLnH` copies `C` → `PARM1` before `B` → `PARM0`, three words each;
- the `FETCH_GBL_*` index is `trunc(B.f)`, bounded by the prefix int, inclusive;
- `DIV_I64`/`DIV_U64` are unguarded;
- `NE_S` stores the raw `strcmp` result;
- the world is read-only after `CSQC_WorldLoaded`;
- FTE also rejects an index > `numfunctions`.
