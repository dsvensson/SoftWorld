<!-- SPDX-License-Identifier: MIT OR Apache-2.0 -->

# FTE string and formatting builtins

This document describes FTEQW behaviour, written from scratch in our own words after studying FTE
as a behavioural specification. No FTE (GPL) code is reproduced here; `file:line` references into
`fteqw/engine/` are pointers for behaviour lookup only.

Where this crate deliberately deviates from FTE, see [deviations.md](deviations.md). The
deviations that concern string builtins are summarised under [FTE quirks](#fte-quirks).

**Notation.** Examples use QuakeC literals: `"…"` is a string with C escapes (`\\` is one
backslash, `\n` a newline, `\xNN` the byte NN), a bare number such as `5` is a float argument and
`5i` an integer. Results are written the same way. The behaviour described is that of FTE on
x86-64 with glibc.

## Shared rules

### Builtin numbers

CSQC numbers; `#0` means the builtin is resolved by name. `strcmp` is `strncmp` under another name
(`#define strcmp strncmp`).

| Area | Builtins |
|---|---|
| Conversion | `ftos` 26, `vtos` 27, `etos` 65, `stof` 81, `stov` 117, `stoi` 259, `itos` 260, `stoh` 261, `htos` 262, `ftoi` #0, `itof` #0 |
| Formatting | `sprintf` 627 |
| Length, substrings, characters | `strlen` 114, `strcat` 115, `substring` 116, `strzone` 118, `strunzone` 119, `strstrofs` 221, `str2chr` 222, `chr2str` 223 |
| Case | `strconv` 224, `strtolower` 480, `strtoupper` 481 |
| Padding, trimming, replacing | `strpad` 225, `strtrim` #0, `strreplace` 484, `strireplace` 485 |
| Comparison | `strncmp` 228, `strcasecmp` 229, `strncasecmp` 230 |
| Colour markup | `strlennocol` 476, `strdecolorize` 477 |
| Info strings | `infoadd` 226, `infoget` 227 |
| URIs | `uri_escape` 510, `uri_unescape` 511 |
| Checksums and digests | `crc16` 494, `digest_hex` 639 |
| Time | `strftime` 478 |
| Tokenizers | `tokenize` 441, `argv` 442, `argc` #0, `tokenizebyseparator` 479, `tokenize_console` 514, `argv_start_index` 515, `argv_end_index` 516 |

### Arguments

- Floats become integers by C truncation (on x86, `INT_MIN` when out of range or NaN).
- The argument count matters: optional parameters are truly optional.
- Some builtins take a tail of strings, which is concatenated from argument *k* on. A single
  argument is used as it is, whatever its length; no argument gives `""`; two or more are joined
  into a 65,543-byte buffer, truncating. The tail starts at argument 0 for `stov`, 1 for `strpad`,
  `crc16`, `digest_hex` and `strftime`, 2 for the value of `infoadd`, and 3 for `strconv`.
- A string argument that is really a float or an integer is read as a string handle: 0 gives
  `""`, an out-of-range handle gives `""` and a warning.

### Results

- Every string result is a new temp string, copied up to the first NUL. An empty result is still
  a non-null temp string.
- Only `argv` out of range and `digest_hex` with an unknown algorithm return null (0).
- `strzone` is `strcat`; `strunzone` does nothing.
- FTE's legacy ring of temp strings (`pr_tempstringcount` ≥ 2) is off by default and ignored.

### Length caps

| Builtin | Limit |
|---|---|
| `strcat`, `strzone`, `substring`, `strtrim`, `argv` | none |
| `ftos`, `itos`, `htos`, `etos`, `vtos` | none (the full string) |
| `sprintf` | 65,535 bytes |
| `strpad` | 4,095 bytes |
| `strconv` | the input is cut to 4,095 bytes |
| `infoadd` | the info string is cut to 4,095 bytes; a pair ≤ 1,023 bytes, a key < 256, the total ≤ 4,096 including the NUL |
| `infoget` | keys and values of 1,022 bytes or more are not found |
| `strreplace`, `strireplace` | stop when the output reaches 4,094 − len(replace) bytes |
| `strtolower`, `strtoupper` | 8,191 bytes (truncated) |
| `strdecolorize` | 8,190 bytes |
| `uri_escape` | 8,188 bytes |
| `uri_unescape` | 8,190 bytes |
| `strftime` | 8,191 bytes |
| tokenizers | 65,535 bytes per token (a longer word continues as the next token); any number of tokens |

### Character sets

FTE configures these with process-global cvars; qcvm makes them per-VM configuration.

- **`utf8_enable`** (default 0): when set, characters are counted instead of bytes by `strlen`,
  `substring`, `str2chr`, `strstrofs`, the offsets and length of `strncmp`, `chr2str` for codes up
  to 255, and the width and precision of `sprintf`'s `%s`, `%S` and `%c`. These always work on
  bytes: `strpad`, `strconv`, `strreplace`, `strtrim`, the tokenizers, `argv_start_index` and
  `argv_end_index`, `uri_escape` and `uri_unescape`, `infoadd` and `infoget`, `crc16` and
  `digest_hex`.
- **`com_parseutf8`** (cvar default 1, but the Quake/QuakeWorld game scheme sets 0 and Hexen 2
  −1): the decoder and encoder used by `strtoupper`/`strtolower`, `strdecolorize`, `chr2str` above
  255, and all UTF-8 decoding in the VM.
- QuakeWorld CSQC runs with `utf8_enable 0` and `com_parseutf8 0`.
- Markup (`^U`, `^{…}`) is not parsed for builtins.

Decoding, by `com_parseutf8`:

| Value | Decoding |
|---|---|
| 0 (Quake) | one byte is one character; bytes 1–31 other than `\t` `\n` `\r`, and bytes ≥ 128, become U+E000 + byte; 127 stays |
| −1 | the byte value |
| ≥ 1 (UTF-8) | a stray continuation byte (0x80–0xBF) or 0xFE/0xFF becomes U+E000 + byte (one byte); a lead byte 0xC0–0xFD without the right continuation bytes becomes U+FFFD (one byte); an overlong form decodes to its value, consuming all its bytes, and `C0 80` to 0; CESU-8 surrogate pairs are combined; 5- and 6-byte forms decode |

Encoding, by `com_parseutf8`:

| Value | Encoding |
|---|---|
| ≥ 1 | the shortest UTF-8, except 0 → `C0 80` |
| 0 | bytes 32–127 and `\t` `\n` `\r` as they are; U+E000–U+E0FF → the low byte, except U+E009, U+E00A, U+E00D and U+E00B; anything else → `?` without markup, or with markup `^U` and four lowercase hex digits (`^{hex}` above U+FFFF) |
| −1 | below 256, or U+E020–U+E07F → the (low) byte; anything else → `?` or markup |

qcvm's Quake encoder round-trips U+E00B (`\v`).

## Number ↔ string

### `ftos`

- If `v` equals its truncation to a 32-bit integer, the result is that integer in decimal (−0 →
  `"0"`).
- Otherwise (the default `pr_brokenfloatconvert 0`) the value is printed exactly:
  - Infinity and NaN: `-` if the sign bit is set (NaN included), then `1.#INF` or `1.#NAN`. On
    x86, 0/0 is a negative NaN (`-1.#NAN`); the sign bit is preserved.
  - Otherwise let *e* be the unbiased binary exponent (raw − 127; −127 for denormals) and
    *N* = 8 + trunc(−*e* · 0.30103), truncated toward zero. If *N* ≤ 0 (|v| ≥ 2²⁷) the value is
    formatted with `%.0f`; else it is formatted as an f64 with exactly *N* decimals, correctly
    rounded (exact ties to even), and trailing zeros and a trailing `.` are stripped.

| Value | `ftos` |
|---|---|
| `0.5` | `"0.5"` |
| `0.1` | `"0.100000001"` |
| `0.2` | `"0.2"` |
| `0.3` | `"0.30000001"` |
| `1.1` | `"1.10000002"` |
| `1/3` | `"0.33333334"` |
| `2/3` | `"0.66666669"` |
| `0.01` | `"0.0099999998"` |
| `0.001` | `"0.00100000005"` |
| `0.00001` | `"0.0000099999997"` |
| `3.14159265` | `"3.14159274"` |
| `10.1` | `"10.10000038"` |
| `16.1` | `"16.1000004"` |
| `99.99` | `"99.9899979"` |
| `123.456` | `"123.4560013"` |
| `1000.5` | `"1000.5"` |
| `123456.7` | `"123456.7031"` |
| `1048576.125` | `"1048576.12"` |
| `16777217` | `"16777216"` |
| `-2147483648` | `"-2147483648"` |
| 2³¹ | `"2147483648"` (x86) |
| `3e9` | `"3000000000"` |
| `1e20` | `"100000002004087734272"` |
| smallest denormal | `"0.0000000000000000000000000000000000000000000014"` |
| +∞, −∞ | `"1.#INF"`, `"-1.#INF"` |
| `-42` | `"-42"` |

### `vtos`

`'`, each component formatted with C's `%f` (six decimals, as an f64) and separated by spaces,
then `'`. NaN and infinity print as glibc prints them (`nan`, `-nan`, `inf`).

| Value | `vtos` |
|---|---|
| `'1 2 3'` | `"'1.000000 2.000000 3.000000'"` |
| `'0.1 -0 1e10'` | `"'0.100000 -0.000000 10000000000.000000'"` |

### `etos`, `itos`, `htos`

- `etos`: `"entity N"`.
- `itos`: signed decimal.
- `htos`: exactly eight lowercase hex digits: `255i` → `"000000ff"`, `-1i` → `"ffffffff"`.

### `stoi`

C's `atoi`: leading whitespace, an optional sign, decimal digits only. Overflow wraps modulo 2³²
(glibc on 64-bit: `"4294967297"` → 1). `"  -12abc"` → −12, `"0x1f"` → 0, `"010"` → 10, `"abc"` →
0.

qcvm takes the base from the prefix, as documented for FTE (`0x` → 16, a leading `0` → 8):
`"0x1f"` → 31, `"010"` → 8.

### `stoh`

`strtoul` with base 16, keeping the low 32 bits: whitespace, a sign, an optional `0x`, hex digits.

| Argument | `stoh` |
|---|---|
| `"1F"` | 31 |
| `"0x1f"` | 31 |
| `"10"` | 16 |
| `"-1"` | −1 |
| `"ffffffff"` | −1 |
| `"zz"` | 0 |

### `ftoi`, `itof`

- `ftoi(f)`: truncates toward zero (x86: `INT_MIN` when out of range or NaN).
- `itof(v)` with one argument: the integer converted to the nearest float (ties to even).
- `itof(v, shift, count = 24)` with two or more arguments: `(u32(v) >> shift) & mask(count)`,
  converted to float; there is no mask when `count` is 32. Shift amounts of 32 or more are masked
  to five bits, as x86 does. `itof(0x12345678i, 8, 8)` → 86; `itof(-1i, 0, 32)` → 4294967296.

### `stof`

C's `atof` in the C locale, as an f64 rounded to f32: skips `isspace` bytes (space, `\t` `\n`
`\v` `\f` `\r`), then a sign, a decimal number with an optional exponent, a C99 hex float
(`"0x10"` → 16, `"0x1p4"` → 16), or `inf`/`infinity`/`nan`/`nan(…)` in any case. Trailing junk
is ignored; no digits gives 0.

| Argument | `stof` |
|---|---|
| `"  12.5xyz"` | 12.5 |
| `".5"` | 0.5 |
| `"1e3"` | 1000 |
| `"1,5"` | 1 |
| `"\"5\""` | 0 |
| `"1e39"` | ∞ |

### `stov`

Start with (0, 0, 0) and skip one leading `'` if it is the first byte. Then, up to three times:

1. skip spaces and tabs (only those);
2. parse a number with `atof` at the current position into the next component;
3. if the value is 0 **and** the current byte is not `-`, `+` or a digit, stop;
4. otherwise advance to the next space, tab, `'` or the end; stop at a `'`.

| Argument | `stov` |
|---|---|
| `"'1 2 3'"` | (1, 2, 3) |
| `"1\t2\t3"` | (1, 2, 3) |
| `"'1 2'"` | (1, 2, 0) |
| `"1,2,3"` | (1, 0, 0) |
| `"1\n2\n3"` | (1, 0, 0) |
| `"1-2 3"` | (1, 3, 0) |
| `"0 1 2"` | (0, 1, 2) |
| `".0 1 2"` | (0, 0, 0) |
| `" '1 2 3'"` | (0, 0, 0) |
| `"(1 2 3)"` | (0, 0, 0) |
| `"1e3 2 3"` | (1000, 2, 3) |
| `"nan 1 2"` | (NaN, 1, 2) |

## `sprintf`

`pr_bgcmd.c:7295-7691`. The output is at most 65,535 bytes; the values start at parameter 1.

### Conversion specifications

After a `%`, in this order:

1. **Position** `N$`: digits immediately after the `%`, followed by `$`; *N* counts the arguments
   after the format from 1.
2. **Width first**: digits right after the `%` without a `$` are the width, and a leading `0`
   among them sets zero padding (`%05d`). When the width comes first, no flags are parsed after it
   (`%5-d` is an error).
3. **Flags**, only if there was no width yet: `#`, `0`, `-`, space and `+`, in any order and
   repeated.
4. **Width** after the flags: digits, `*` or `*N$`. A `*` width is **always read as a float** (an
   integer argument's bits are about 0); a negative width left-aligns with its absolute value.
5. **Precision**: `.` followed by digits, `*` or `*N$`. A `.` alone (`%.f`) is a format error. A
   `*` precision is read as a float; a negative precision means none.
6. **Length modifiers**, repeatable: `h` → a float argument; `l` or `L` → an int32 argument (`ll`
   too); `q` → a 64-bit value over two slots (a double, or an int64 with `l`); `j`, `z` and `t`
   are ignored.
7. **Conversion**, below.

Arguments are float by default, except that `%i`, `%p` and `%P` default to int. They are
consumed left to right, `*` width and precision before the value; positional arguments do not
advance the running position.

### Conversions

| Conversion | Output |
|---|---|
| `d`, `i` | signed: a float is truncated, an int widened to i64; a float out of range or NaN gives `i64::MIN` |
| `u`, `o`, `x`, `X` | unsigned 64-bit: an int is zero-extended from 32 bits; a negative float wraps (−1 → `ffffffffffffffff`) |
| `p`, `P` | like `x`/`X`, always zero-padded, default width 8, default type int |
| `e`, `E`, `f`, `F`, `g`, `G` | C formatting of the value as an f64 |
| `v`, `V` | the three components, each as `%g`/`%G` with the same flags, width and precision, separated by single spaces, without quotes (`l` → integer components) |
| `c` | with `utf8_enable 0` (or the `#` flag): C's `%c` of the low byte of the truncated value, where 0 prints nothing; with `utf8_enable 1`: the encoded code point, width and precision in characters |
| `s` | width and precision in bytes (in characters if `utf8_enable 1` and `com_parseutf8` > 0) |
| `S` | quoted: if the string contains a newline, a carriage return or `"`, then `\"`, the string with newline, carriage return, tab, `'`, `"`, `\` and `$` escaped by a backslash (the first three as `\n`, `\r`, `\t`), and `"`; otherwise `"`, the string, `"` (with `dpcompat_console`, only `\` and `"` are escaped) |
| `%` | a literal `%` |

- `#` is dropped for `s`, `c` and `S`.
- `%0Ns` pads strings with spaces (glibc).
- Anything else is a **format error**: output stops there, keeping the text so far, with a warning.
  That covers unknown conversions (`%k`, `%a`, `%n`), a trailing `%`, a bad `.` and `%5-d`. FTE
  passes `%I` to the C library; qcvm treats it as a format error too.
- Missing arguments print as 0, `""` or `'0 0 0'`; extra arguments are ignored. `%s` given a float
  reinterprets its bits as a string handle.

### Examples

| Call | Result |
|---|---|
| `("%d", 3.7)` | `"3"` |
| `("%d", -3.7)` | `"-3"` |
| `("%d", 1e10)` | `"10000000000"` |
| `("%d", 5i)` | `"0"` |
| `("%ld", 5i)` | `"5"` |
| `("%i", 5i)` | `"5"` |
| `("%i", 5.0)` | `"1084227584"` |
| `("%hi", 5.0)` | `"5"` |
| `("%lf", 5i)` | `"5.000000"` |
| `("%+d", 42)` | `"+42"` |
| `("% d", 42)` | `" 42"` |
| `("%-6d\|", 42)` | `"42    \|"` |
| `("%06.2f", -1.5)` | `"-01.50"` |
| `("%5.2f\|", 3.14159)` | `" 3.14\|"` |
| `("%x", 255)` | `"ff"` |
| `("%X", 255.9)` | `"FF"` |
| `("%#x", 255)` | `"0xff"` |
| `("%08x", 255)` | `"000000ff"` |
| `("%x", 255i)` | `"0"` |
| `("%lx", -1i)` | `"ffffffff"` |
| `("%p", 255i)` | `"000000ff"` |
| `("%o", 8)` | `"10"` |
| `("%e", 12345)` | `"1.234500e+04"` |
| `("%g", 0.1)` | `"0.1"` |
| `("%g", 1e20)` | `"1e+20"` |
| `("%f", 0.1)` | `"0.100000"` |
| `("%.10f", 0.1)` | `"0.1000000015"` |
| `("%c", 65)` | `"A"` |
| `("%c", 321)` | `"A"` |
| `("%3c", 65)` | `"  A"` |
| `("%c", 0)` | `""` |
| `("%5s\|", "ab")` | `"   ab\|"` |
| `("%-5s\|", "ab")` | `"ab   \|"` |
| `("%.1s", "ab")` | `"a"` |
| `("%S", "a b")` | `"\"a b\""` |
| `("%S", "say \"hi\"")` | `"\\\"say \\\"hi\\\"\""` |
| `("%S", "x\ny")` | `"\\\"x\\ny\""` |
| `("%v", '1 2 3')` | `"1 2 3"` |
| `("%.3v", '1.23456 0 -0.5')` | `"1.23 0 -0.5"` |
| `("%5v", '1 2 3')` | `"    1     2     3"` |
| `("%#v", '1 2 3')` | `"1.00000 2.00000 3.00000"` |
| `("%2$s-%1$s", "a", "b")` | `"b-a"` |
| `("%2$s %s", "a", "b")` | `"b a"` |
| `("%*d", 5, 42)` | `"   42"` |
| `("%*d\|", -5, 42)` | `"42   \|"` |
| `("%.*f", 2, 3.14159)` | `"3.14"` |
| `("%*d", 5i, 42)` | `"42"` |
| `("%d %s")` | `"0 "` |
| `("%v")` | `"0 0 0"` |
| `("%%")` | `"%"` |
| `("ab%kcd")` | `"ab"` |
| `("x%")` | `"x"` |
| `("A%.f", 1)` | `"A"` |
| `("%5-d", 1)` | `""` |

## Length, concatenation, substrings, characters

### `strlen`

Bytes with `utf8_enable 0`, otherwise decoding steps; null → 0.

### `strcat`, `strzone`

Concatenate arguments 0 to argc − 1 (at most 8); no arguments give `""`; no length limit.

### `substring(s, start, len)`

*slen* is the length in bytes (in characters in UTF-8 mode).

1. If `start` < 0: `start += slen`.
2. If `len` < 0: `len = slen − start + len + 1`, using the new `start` (−1 means to the end).
3. If `start` < 0: `start = 0`; `len` is **not** reduced.
4. If `start` ≥ *slen* or `len` ≤ 0: `""`.
5. Clamp `len` to *slen* − `start`.

| Call | Result |
|---|---|
| `("hello", 1, 3)` | `"ell"` |
| `("hello", -3, 2)` | `"ll"` |
| `("hello", 1, -1)` | `"ello"` |
| `("hello", 0, -2)` | `"hell"` |
| `("hello", -10, 3)` | `"hel"` |
| `("hello", -10, -1)` | `"hello"` |
| `("hello", 3, 100)` | `"lo"` |
| `("hello", 5, 1)` | `""` |
| `("hello", 2, 0)` | `""` |
| `("hello", 4.9, 1)` | `"o"` |
| `("hello", 0, 3e9)` | `""` (`INT_MIN`) |

### `strstrofs(s, sub, start = 0)`

A non-zero `start` that is negative or beyond `strlen(s)` gives −1. Otherwise the search starts at
`start` and returns the absolute byte offset of the match, or −1; an empty `sub` matches at
`start`.

| Call | Result |
|---|---|
| `("abcabc", "bc")` | 1 |
| `("abcabc", "bc", 2)` | 4 |
| `("abc", "")` | 0 |
| `("abc", "", 3)` | 3 |
| `("abc", "", 4)` | −1 |
| `("abc", "a", -1)` | −1 |
| `("abc", "x")` | −1 |

### `str2chr(s, idx = 0)`

A negative index counts from the end. A non-zero index out of range (negative or beyond the length)
gives 0, and so does an index equal to the length. The result is the unsigned byte in byte mode,
the code point in UTF-8 mode.

| Call | Result |
|---|---|
| `("abc", 0)` | 97 |
| `("abc", 2)` | 99 |
| `("abc", 3)` | 0 |
| `("abc", 4)` | 0 |
| `("abc", -1)` | 99 |
| `("abc", -3)` | 97 |
| `("abc", -4)` | 0 |
| `("abc", 1.9)` | 98 |
| `("abc", -0.5)` | 97 |
| `("\xE1", 0)` | 225 |
| `("\xE1")` with `utf8_enable 1`, `com_parseutf8 0` | 0xE0E1 |

### `chr2str(c, …)`

- `utf8_enable 0` and a code ≤ 255: the raw byte (0 ends the string; a negative code gives a byte
  0x80–0xFF, as on x86).
- A code > 255, in either mode: encoded **with** markup.
- `utf8_enable 1` and a code ≤ 255: encoded without markup.

| Call | Settings | Result |
|---|---|---|
| `(72, 105)` | QuakeWorld defaults | `"Hi"` |
| `(0xE1)` | QuakeWorld defaults | `"\xE1"` |
| `(0x263A)` | QuakeWorld defaults | `"^U263a"` |
| `(256)` | QuakeWorld defaults | `"^U0100"` |
| `(0x1F600)` | QuakeWorld defaults | `"^{1f600}"` |
| `(0xE0C1)` | QuakeWorld defaults | `"\xC1"` |
| `(0xE00A)` | QuakeWorld defaults | `"^Ue00a"` |
| `(65, 0, 66)` | QuakeWorld defaults | `"A"` |
| `(0x263A)` | `com_parseutf8 1` | `"\xE2\x98\xBA"` |
| `(0xE041)` | `com_parseutf8 1` | `"\xEE\x81\x81"` |
| `(200)` | `utf8_enable 1`, `com_parseutf8 0` | `"?"` |
| `(0)` | `utf8_enable 1`, `com_parseutf8 0` | `"?"` |
| `(0)` | `utf8_enable 1`, `com_parseutf8 1` | `"\xC0\x80"` |

## Case

### `strtoupper`, `strtolower`

Decode with the `com_parseutf8` decoder, whatever `utf8_enable` says; map case for ASCII only (C
locale), and for U+E020–U+E07F on the low 7 bits; re-encode without markup.

- Quake scheme: ASCII letters change; everything else round-trips, except that byte 0x0B becomes
  `?`; red letters do not change. `"Hello, World!"` → `"HELLO, WORLD!"`, `"\xE1bc"` →
  `"\xE1BC"`, `"a\vb"` → `"A?B"`.
- UTF-8 scheme: `"héllo"` → `"HéLLO"`, a stray `"\x80"` → `"\xEE\x82\x80"`, `"\xE1bc"` →
  `"\xEF\xBF\xBDBC"`, `"\xC1\xA1"` → `"A"` (upper case).

qcvm leaves byte 0x0B unchanged: `"a\vb"` → `"A\vB"`.

### `strconv(ccase, redalpha, redchars, str…)`

Works on bytes, at most 4,095. Each byte *b* at index *i* is handled by the first rule that
matches:

| Bytes | Handling |
|---|---|
| digits: white 0x30–0x39, red 0xB0–0xB9, gold-high 0x92–0x9B, gold-low 0x12–0x1B | by `redchars`, keeping the digit *d*: 1 → 0x30 + *d*, 2 → 0xB0 + *d*, 3 → 0x12 + *d*, 4 → 0x92 + *d*; anything else (5 and 6 included) → unchanged |
| letters: lower white 0x61–0x7A, upper white 0x41–0x5A, lower red 0xE1–0xFA, upper red 0xC1–0xDA | case by `ccase` (1 lower, 2 upper, else kept), then colour by `redalpha`: 1 white, 2 red, 5 red if *i* is even else white, 6 red if *i* is odd else white, else kept (*i* counts every byte) |
| `(b & 0x7F) < 0x10`, and any other byte when `redalpha` is 0 | unchanged |
| other punctuation | by `redalpha`: 1 → `b & 0x7F`, 2 → `b \| 0x80`, else unchanged |

| Call | Result |
|---|---|
| `(1, 0, 0, "Hello WORLD")` | `"hello world"` |
| `(2, 0, 0, "\xE8i")` | `"\xC8I"` |
| `(0, 2, 2, "Score: 10")` | `"\xD3\xE3\xEF\xF2\xE5\xBA\xA0\xB1\xB0"` |
| `(0, 1, 1, "\xD3\xE3\xEF\xF2\xE5\xBA\xA0\xB1\xB0")` | `"Score: 10"` |
| `(0, 0, 3, "123")` | `"\x13\x14\x15"` |
| `(0, 0, 4, "05")` | `"\x92\x97"` |
| `(0, 5, 0, "ab cd")` | `"\xE1b c\xE4"` |
| `(0, 6, 0, "abcd")` | `"a\xE2c\xE4"` |

## Padding, trimming, replacing

### `strpad(pad, str…)`

Works on bytes. For `pad` ≥ 0: the string, then max(0, min(`pad`, 4095) − len) spaces. For `pad`
< 0: max(0, min(−`pad` − len, 4095)) spaces, then the string, at most 4,095 bytes in total.

| Call | Result |
|---|---|
| `(5, "ab")` | `"ab   "` |
| `(-5, "ab")` | `"   ab"` |
| `(2, "abcd")` | `"abcd"` |
| `(-3, "a", "b")` | `" ab"` |
| `(-5.5, "a")` | `"    a"` |

FTE returns 4,095 spaces when −`pad` is shorter than the string; qcvm returns the string unpadded,
as the formula says.

### `strtrim`

Strips space, `\t`, `\n` and `\r` at both ends (not `\v` or `\f`): `"  a b \n"` → `"a b"`,
`"\v x"` → `"\v x"`.

### `strreplace`, `strireplace(search, replace, subject)`

An empty `search` returns `subject` unchanged. Otherwise the subject is scanned left to right for
non-overlapping matches; replacements are not scanned again. The output is capped (see
[length caps](#length-caps)), so a `replace` of about 4,094 bytes or more gives `""`.
`strireplace` matches ASCII case-insensitively and inserts the replacement verbatim.

| Call | Result |
|---|---|
| `strreplace("a", "bb", "banana")` | `"bbbnbbnbb"` |
| `strreplace("aa", "b", "aaa")` | `"ba"` |
| `strireplace("AB", "x", "abAb")` | `"xx"` |

## Comparison

### `strncmp(s1, s2, [len, s1ofs, s2ofs])`

- Two arguments: C's `strcmp` on unsigned bytes, returning the byte difference (glibc x86-64:
  `("a", "c")` → −2).
- Three or more: `strncmp(s1 + s1ofs, s2, len)`. A negative `len` means unlimited, a `len` of 0
  gives 0, and a negative or too large `s1ofs` is clamped to the length. **FTE range-checks
  `s2ofs` but never applies it.** In UTF-8 mode the offsets count characters, and `len` becomes
  the larger byte span of `len` characters in either string.

| Call | Result |
|---|---|
| `("hello", "help", 3)` | 0 |
| `("hello", "help", 4)` | negative |
| `("xxhello", "hello", 5, 2)` | 0 |
| `("hello", "xxhel", 3, 0, 2)` | non-zero (FTE's bug; qcvm: 0) |

qcvm applies `s2ofs`.

### `strcasecmp`, `strncasecmp`

One function under two names.

- Up to two arguments: a full comparison returning exactly −1, 0 or +1. ASCII a–z are folded to
  **upper** case, and bytes are compared as **signed** chars (0x80–0xFF are negative).
- Three or more: limited to `len`, with both `s1ofs` **and** `s2ofs` applied (clamped like
  `s1ofs` above); a negative `len` means unlimited.

| Call | FTE | qcvm |
|---|---|---|
| `strcasecmp("abc", "ABC")` | 0 | 0 |
| `strcasecmp("a", "b")` | −1 | −1 |
| `strcasecmp("b", "A")` | 1 | 1 |
| `strcasecmp("_", "a")` | 1 | −1 |
| `strcasecmp("abc", "abcd")` | −1 | −1 |
| `strcasecmp("\xE9", "a")` | −1 | 1 |
| `strncasecmp("abc", "ABD", 2)` | 0 | 0 |
| `strncasecmp("xxABC", "abc", 3, 2)` | 0 | 0 |

qcvm has C semantics: it folds to lower case and compares unsigned bytes.

## `strdecolorize`, `strlennocol`

The string is parsed into (character, style) pairs and re-encoded without the styles: markup is
removed, link payloads are dropped. There is no bidi handling.

### Markup

| Input | Result |
|---|---|
| 0x01 or 0x02 as the first byte (with `dpcompat_console` also 0x03) | removed |
| `^0` … `^9` | removed |
| `^&XY`, X and Y each one of 0–9, A–F (upper case only) or `-` | removed; if invalid, the `^` is kept literally and parsing continues at the `&` |
| `^xRGB`, three hex digits in either case | removed; otherwise only the `^x` is removed |
| `^b` `^d` `^m` `^a` `^h` `^s` `^r` | removed |
| `^^` | `^` |
| `^Uhhhh` | the code point; always consumes six bytes, without checking for hex digits |
| `^{hex}` | the code point (above 0x10FFFF: U+FFFD); the closing `}` is optional |
| `` ^`u8:…`= `` | the text, forced to UTF-8 |
| `^[text\key\val^]` | `text` |
| `^[` without a closing `^]` | `[` |
| a lone `^]` | `]` |
| `^` followed by any other byte, or at the end | kept literally |

ezQuake markup (`ezcompat_markup 1`):

| Input | Result |
|---|---|
| `&cRGB`, three hex digits | removed; otherwise literal |
| `&r` | **always** removed |
| `` =`k8:…`= `` | the text, decoded as KOI8 |

qcvm requires four hex digits after `^U` and leaves it literal otherwise.

### Characters

- Quake scheme: `\t` `\n` `\r` `\v`, space and 32–126 are kept; **0xA0–0xFF lose their high
  bit**; 0x80–0x9F, 0x7F and other control bytes are kept.
- UTF-8 scheme: valid sequences are kept. At the first malformed byte the rest of the string
  switches to the Quake rules: 0xA0–0xFF lose their high bit, and 0x80–0x9F, 0x7F and control
  bytes become the 3-byte UTF-8 of U+E0xx.

### Examples (QuakeWorld settings)

| Argument | `strdecolorize` |
|---|---|
| `"^1Red ^7White"` | `"Red White"` |
| `"^^1"` | `"^1"` |
| `"a^"` | `"a^"` |
| `"^z"` | `"^z"` |
| `"^&F0text"` | `"text"` |
| `"^&G0x"` | `"^&G0x"` |
| `"^xF00red"` | `"red"` |
| `"^xZZ"` | `"ZZ"` |
| `"^[link\\url\\http://x^]"` | `"link"` |
| `"^[abc"` | `"[abc"` |
| `"&cf00red&r"` | `"red"` |
| `"rock&roll"` | `"rockoll"` |
| `"\x01hi"` | `"hi"` |
| `"\xC8\xE9"` | `"Hi"` |
| `"^U0041"` | `"A"` |
| `"^U263a"` | `"?"` (with `com_parseutf8 1`: `"\xE2\x98\xBA"`) |
| `"^b^m^h"` | `""` |

`strlennocol` is the length in bytes of the `strdecolorize` result.

## Info strings

### `infoget(info, key)`

An optional leading `\`, then keys and values alternating, separated by `\`. Keys are
case-sensitive. The result is a copy of the value, or `""`; an odd number of fields gives `""`,
and keys or values of 1,022 bytes or more are not found.

| Call | Result |
|---|---|
| `("\\name\\bob\\team\\red", "team")` | `"red"` |
| `("name\\bob", "name")` | `"bob"` |
| `("\\name\\bob", "Name")` | `""` |
| `("\\a\\\\b\\2", "b")` | `"2"` |
| `("\\a\\1\\b", "b")` | `""` |

### `infoadd(info, key, value…)`

The value is the concatenation of the arguments from the third on; the info string is cut to 4,095
bytes first.

1. Nothing changes (with a warning) if the key or the value contains `\` or `"`, or if the key is
   256 bytes or longer.
2. If the key gets a non-empty value and the new total would exceed 4,096 bytes: remove `*ver`, if
   present, and try again; otherwise nothing changes.
3. Remove the **first** occurrence of the key.
4. An empty value stops here (the key is deleted).
5. Cut the pair `\key\value` to 1,023 bytes; if the total would then exceed 4,096, stop (the old
   pair is already gone).
6. Append the pair at the end, dropping bytes ≤ 13.

Star keys are allowed.

| Call | Result |
|---|---|
| `("", "name", "bob")` | `"\\name\\bob"` |
| `("\\name\\bob", "team", "red")` | `"\\name\\bob\\team\\red"` |
| `("\\name\\bob\\team\\red", "name", "al")` | `"\\team\\red\\name\\al"` |
| `("\\name\\bob\\team\\red", "name", "")` | `"\\team\\red"` |
| `("\\name\\bob", "x", "a\\b")` | `"\\name\\bob"` (unchanged) |
| `("\\name\\bob", "x", "a\nb")` | `"\\name\\bob\\x\\ab"` |

qcvm returns the info string unchanged when step 5 rejects the pair.

## URIs

### `uri_escape`

Keeps `A–Z`, `a–z`, `0–9`, `.`, `-`, `_` and `~`; every other byte becomes `%` and two
**upper-case** hex digits. `"a b"` → `"a%20b"`, `"100%"` → `"100%25"`, `"é"` (UTF-8) →
`"%C3%A9"`, `"~x"` → `"~x"`.

### `uri_unescape`

`%` and two hex digits (either case) become that byte; any other `%` is copied and decoding
continues. `+` is left alone, and a decoded `%00` ends the string.

| Argument | `uri_unescape` |
|---|---|
| `"%41"` | `"A"` |
| `"%4"` | `"%4"` |
| `"%zz"` | `"%zz"` |
| `"%%41"` | `"%A"` |
| `"a%00b"` | `"a"` |
| `"a+b"` | `"a+b"` |

## `crc16`, `digest_hex`

### `crc16(caseinsensitive, s…)`

Returns a float: CRC-16/CCITT-FALSE (polynomial 0x1021, not reflected, initial value 0xFFFF, no
final XOR). With `caseinsensitive`, each byte is lower-cased (ASCII) first.

| Call | Result |
|---|---|
| `(0, "")` | 65535 |
| `(0, "A")` | 47381 (0xB915) |
| `(0, "123456789")` | 10673 (0x29B1) |
| `(1, "ABC")` | the same as `(0, "abc")` |

### `digest_hex(alg, data…)`

The algorithm names are exact and case-sensitive: `MD4`, `MD5`, `SHA1`, `SHA2-224`/`SHA224`,
`SHA2-256`/`SHA256`, `SHA2-384`/`SHA384`, `SHA2-512`/`SHA512` and `CRC16`. The result is the
digest in lowercase hex; an unknown algorithm gives **null**. The CRC16 digest's bytes are
little-endian.

| Call | Result |
|---|---|
| `("CRC16", "123456789")` | `"b129"` |
| `("CRC16", "A")` | `"15b9"` |
| `("CRC16", "")` | `"ffff"` |
| `("MD5", "")` | `"d41d8cd98f00b204e9800998ecf8427e"` |
| `("MD4", "")` | `"31d6cfe0d16ae931b73c59d7e0c089c0"` |
| `("SHA1", "")` | `"da39a3ee5e6b4b0d3255bfef95601890afd80709"` |
| `("SHA256", "abc")` | `"ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"` |

## `strftime(uselocaltime, format…)`

Formats the current time, local if the first argument is non-zero, else UTC. The format is the
concatenation of the remaining arguments, passed to C's `strftime` in the C locale (English), with
at most 8,191 bytes of output. A format of exactly `"%R"` becomes `"%H:%M"`, and exactly `"%F"`
becomes `"%Y-%m-%d"` (only when that is the whole format). Unknown specifiers are printed
literally (MSVC would abort).

## Tokenizers

There is one token list — process-global in FTE, **per VM** here — and every tokenizer replaces
it. Each token has its text and its start and end byte offsets; `argv` returns a new temp copy.

### `tokenize` (QuakeC mode)

1. Skip bytes ≤ 0x20, newlines included; the token starts after them.
2. `//` at the start of a token skips to the end of the line. If a newline follows, it becomes a
   `"\n"` token (starting at the `/`); a comment that runs to the end gives no token.
3. `\"` at the start: a C-style quoted string. `\n`, `\t` and `\r`; `\xH` and `\xHH`; `\$`, `\\`,
   `\'` and `\"` are literal; a backslash-newline continues the line; any other escape becomes
   `?`. A `\x` without hex digits, or `\x00`, ends the token.
4. `"` at the start: quoted up to the next `"`; a doubled `""` is a literal `"`; unterminated →
   to the end.
5. `'` at the start: single-quoted up to the next `'`. FTE's behaviour for an unterminated string
   or a doubled `''` is undefined; the end of the string terminates it, and a doubled `''` is a
   literal `'`.
6. One of `{ } ( ) [ ] ' : , ;` is a one-character token.
7. Anything else is a word, up to a byte ≤ 0x20 or a step-6 character. `"`, `=`, `/` and high
   bytes are word characters, and `//` inside a word is not a comment.
8. `/* */` comments are not recognised.
9. There is no `$cvar` expansion.

### `tokenize_console`

The same, without the one-character tokens and single-quoted strings. `/* … */` is skipped (to the
end if unterminated). With `dpcompat_console`, quoted strings take `\"` and `\\` escapes and no
`""` doubling.

### Examples

| Argument | Tokens |
|---|---|
| `"say hello world"` | 3 |
| `"  a   b "` | 2 |
| `""`, `"   "` | 0 |
| `"\"hello world\" foo"` | `hello world`, `foo` (token 0 starts at 0 and ends at 13) |
| `"  ab \"c d\" e"` | offsets (2, 4), (5, 10), (11, 12) |
| `"a\"b c\""` | `a"b`, `c"` |
| `"\"a\"\"b\""` | `a"b` |
| `"\"\""` | one empty token |
| `"f(x,y)"` | 6 (`tokenize_console`: 1) |
| `"key:value"` | 3 |
| `"{a;b}"` | 5 |
| `"'1 2 3'"` | 1: `1 2 3` |
| `"say 'hi there'"` with `tokenize_console` | `say`, `'hi`, `there'` |
| `"a // b"` | 1 |
| `"a //x\nb"` | `a`, a newline, `b` |
| `"a\nb"` | `a`, `b` |
| `"a//b"` | 1 |
| `"http://foo"` | `http`, `:` (`tokenize_console`: 1) |
| `"a /*b*/ c"` | 3 (`tokenize_console`: `a`, `c`) |

End offsets: a word ends at the first byte after it; a quoted string just past its closing quote
(or at the end of the string if unterminated); a one-character token just after it.

### `tokenizebyseparator(s, sep1, …, sep7)`

- At most seven separators. An empty input gives no tokens, no separators one token (the whole
  string).
- The string is scanned byte by byte; at each position the separators are tried in argument order,
  and the first match ends the token (its end is the separator's start). The next token starts
  after the separator; the end of the string ends the last token, which may be empty. Nothing is
  trimmed.
- **An empty separator makes FTE loop forever; qcvm ignores empty separators.**

| Call | Tokens |
|---|---|
| `("a,b,,c", ",")` | `a`, `b`, empty, `c` |
| `("a,", ",")` | `a`, empty |
| `(",", ",")` | empty, empty |
| `("abc", ",")` | 1 |
| `("", ",")` | 0 |
| `("a::b:c", "::", ":")` | `a`, `b`, `c` |
| `("a::b", ":", "::")` | `a`, empty, `b` |

### `argv(i)`, `argv_start_index(i)`, `argv_end_index(i)`, `argc()`

`i` is truncated; a negative index counts from the end (−1 is the last token). Out of range,
`argv` returns **null** and the index functions −1. `argc` is the number of tokens; offsets are in
bytes.

## FTE quirks

qcvm reproduces FTE's observable behaviour (x86-64, glibc) except for clear bugs, undefined
behaviour, hangs and buffer overflows; [deviations.md](deviations.md) is the complete list. For
the builtins in this document:

| Quirk | qcvm |
|---|---|
| `strncmp` ignores `s2ofs` | fixed: applies it |
| `strcasecmp` folds to upper case and compares signed chars | fixed: C semantics |
| `strtoupper`/`strtolower` turn `\v` into `?`; the Quake encoder cannot encode U+E00B | fixed: `\v` round-trips |
| `strdecolorize` consumes `^U` without checking for hex digits | fixed: needs four hex digits |
| `stoi` is decimal only | fixed: base from the prefix |
| `infoadd` drops the old pair when it rejects the new one for size | fixed: returns the string unchanged |
| `tokenizebyseparator` with an empty separator loops forever | fixed: ignores it |
| unterminated or doubled `'` in `tokenize` read past the end | fixed: see [Tokenizers](#tokenizers) |
| a `\` at the very end of a `\"…` token reads past the end | fixed: ends the token |
| `strpad` with a negative pad shorter than the string returns 4,095 spaces | fixed: the string unpadded |
| `chr2str` and `%c` with a code ≥ 2³¹ loop forever (UTF-8) | fixed: U+FFFD |
| 6-byte UTF-8 sequences are checked for only four continuation bytes | fixed: all five |
| nested `` ^`u8: `` and `` =`k8: `` markup recurses without bound | fixed: at most 16 levels |
| `sprintf` passes `%I` to the C library | fixed: a format error |
| fixed-size output buffers overflow | fixed: full results, subject to the length caps |
| `substring` with an out-of-range length (`3e9` becomes `INT_MIN`) | kept |
| `ftos` of 2³¹ and the sign of NaN (x86) | kept |
| `strdecolorize` strips every `&r` | kept |
| `uri_escape` keeps `~` | kept |
| the CRC16 digest is little-endian | kept |
| `sprintf` reads `*` and `%d` as floats; `%s` on a non-string reinterprets its bits | kept |
