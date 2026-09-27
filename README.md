# SoftWorld

QuakeWorld with a software renderer, grown from id Software's 1999 source release into a
current Windows program: a client that hosts its own server, a dedicated server, and the
protocol extensions today's servers and clients speak.

- **Renderer:** software only, drawing 32-bit HDR pixels. The render size is 320×200 times a
  whole number, presented through Direct3D 11 (HDR output on HDR displays). Colored lighting
  (`.lit`, BSPX), BSP2 maps, translucency, and AVX-512 kernels.
- **Network:** the FTE, MVD1 and ZQuake extensions (float coordinates, 2048 entities, 4096
  models, chunked downloads, …), mvdsv's player movement and its `pm_` keys.
- **Demos:** QWD and MVD playback, MVD seeking (`demo_jump`), QTV (`qtvplay`), item timers.

## Building

You need Windows 10 or 11 on x64, and:

- Visual Studio 2026 with the C++ workload. It provides MSVC, CMake, Ninja and the Windows
  SDK.
- For the clang build, Visual Studio's LLVM (clang-cl) component.
- Optionally, fteqcc: found on the PATH, or named by the `FTEQCC` environment variable when
  configuring, it compiles the server's game (`qw-qc`). Without it, the `qwprogs.dat` in
  `qw-qc` is used as it is.

Build from a Visual Studio developer prompt (x64):

```
cmake --preset msvc-v4
cmake --build --preset msvc-v4
ctest --preset msvc-v4
```

| Preset | Compiler | Instructions |
|---|---|---|
| `msvc-v4` | MSVC | x86-64-v4 (AVX-512) |
| `clangcl-v4` | clang-cl with lld-link | x86-64-v4 |
| `msvc-generic` | MSVC | baseline x86-64 |
| `msvc-asan` | MSVC, AddressSanitizer | x86-64-v4, Debug only |

`SW_ARCH` (`x86-64-v4` or `generic`) chooses the instruction set: the v4 builds use AVX-512 and
refuse to start on a CPU without it. Builds treat warnings as errors
(`SW_WARNINGS_AS_ERRORS`). The build presets build Release; `msvc-v4-debug` and
`clangcl-v4-debug` build Debug. The `msvc-v4-maps` test preset loads every map under the
directory the `SW_BASEDIR` environment variable names, and is skipped without it.

## Programs

The executables land in `build/<preset>/<config>/`.

- **`softworld`:** the client, and a server of its own for `map`.
- **`softworld-client`:** the client without a server.
- **`softworld-server`:** the dedicated server, in a console.

The two with a server carry its game inside them: a game directory without a `qwprogs.dat`
runs the built-in one.

## Running

The programs need the game data: `id1/pak0.pak` and `pak1.pak` from Quake. Point `-basedir` at
the directory that holds `id1` (and `qw`, and mod directories):

```
softworld -basedir C:\quake +map dm4
softworld -basedir C:\quake +connect qw.example.com
softworld -basedir C:\quake +playdemo mydemo.mvd
softworld -basedir C:\quake +qtvplay 1@qtv.example.com:27599
softworld-server -basedir C:\quake -port 27500 +map dm4
```

Worth knowing:

| | |
|---|---|
| `-scale n`, `vid_scale` | render at 320×200 times n; 0 picks the largest that fits the window |
| `vid_widescreen`, `vid_crt` | wider view (hor+); CRT pixel aspect |
| `r_lightmode` | 1 linear light in RGB, brighter than white where it is (the default); 0 lighting as Quake had it |
| `r_fullbright_scale`, `r_dlight_scale` | fullbrights' light, and dynamic lights' on surfaces, times these |
| `r_threads` | threads drawing the view; 0 (the default) one a core, at most 8 |
| `gamma`, `vid_contrast` | the view's gamma and contrast; the HUD keeps its own |
| `vid_hdr`, `vid_hdr_paperwhite` | HDR output on an HDR display; SDR white's brightness in nits |
| `r_profile 1`, `r_profile_show` | time a frame takes, by stage |
| `cl_maxfps` | frame rate cap; 0 is none but the display's |
| `demo_speed`, `pause` | MVD playback speed, and pause |
| `demo_jump [+\|-][m:]s` | seek in an MVD |
| `track [name]`, jump/attack | follow a player in an MVD or QTV |
| `demo_itemtimers`, `demo_itemrings` | KTX's item announcements, as a list and as rings on the floor |
| `memstats` | memory by use |

## License

GPL version 2, as id released it; see `gnu.txt`. id's original notes are in `docs/original`.
