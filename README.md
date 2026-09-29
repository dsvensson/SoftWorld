# SoftWorld

QuakeWorld with a software renderer, grown from id Software's 1999 source release into a
current Windows, macOS and Linux program: a client that hosts its own server, a dedicated
server, and the protocol extensions today's servers and clients speak.

- **Renderer:** software only, drawing 32-bit HDR pixels. The render size is 320×200 times a
  whole number, presented through Direct3D 11 on Windows, Metal 4 on macOS and Vulkan on
  Linux (Wayland); on macOS and Linux the renderer draws straight into memory the GPU reads (HDR
  output on HDR displays, all three). Colored lighting (`.lit`, BSPX), BSP2 maps, translucency,
  and AVX-512 and NEON kernels.
- **Network:** the FTE, MVD1 and ZQuake extensions (float coordinates, 2048 entities, 4096
  models, chunked downloads, …), mvdsv's player movement and its `pm_` keys.
- **Demos:** QWD and MVD playback, MVD seeking (`demo_jump`), QTV (`qtvplay`), item timers.

## Building

### Windows

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

### macOS

You need a Mac with Apple silicon on macOS 26 or later (Metal 4), and:

- Xcode 26 or later, with its Metal Toolchain component, which compiles the shader:
  `xcodebuild -downloadComponent MetalToolchain`. The Command Line Tools alone don't have it;
  the build finds Xcode's at `/Applications/Xcode.app` (`SW_XCODE_DEVELOPER_DIR` if elsewhere).
- CMake 3.28 or later and Ninja (Homebrew's, for example).
- Optionally fteqcc, as on Windows.

```
cmake --preset macos-m3
cmake --build --preset macos-m3
ctest --preset macos-m3
```

| Preset | Instructions |
|---|---|
| `macos-m3` | Apple M3's (the M2 and later run it), NEON kernels |
| `macos-generic` | any Apple silicon, scalar kernels |

`SW_ARCH` is `apple-m3` or `generic`; the m3 build refuses to start on an M1. `-debug` build
presets build Debug, and `-maps` test presets load the maps as on Windows. The tests include
`test_present`, which draws the Metal shader on the GPU and compares it with what screenshots
make.

### Linux

You need a Wayland compositor (Hyprland, KDE Plasma, Sway and the like; GNOME draws no title bar
for the window), a GPU with Vulkan 1.3, and:

- clang with lld (the presets' compiler and linker), or GCC; CMake 3.28 or later and Ninja.
- Vulkan's headers and loader, glslang (which compiles the shader), and the development files of
  wayland-client, wayland-protocols 1.41 or later, xkbcommon and PipeWire. On Arch:
  `pacman -S cmake ninja clang lld vulkan-headers vulkan-icd-loader glslang wayland
  wayland-protocols libxkbcommon libpipewire`.
- Optionally fteqcc, as on Windows.

```
cmake --preset linux-v4
cmake --build --preset linux-v4
ctest --preset linux-v4
```

| Preset | Instructions |
|---|---|
| `linux-v4` | x86-64-v4 (AVX-512) |
| `linux-generic` | baseline x86-64 or arm64, scalar kernels |

`-debug` build presets build Debug, and `-maps` test presets load the maps as on Windows. The
tests include `test_present_vulkan`, which draws the shader with Vulkan and compares it with what
screenshots make (skipped without a GPU).

## Programs

The executables land in `build/<preset>/<config>/`; on macOS the two with a client are
applications (`softworld.app`), whose programs run from a terminal too
(`softworld.app/Contents/MacOS/softworld`).

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

On macOS the applications run in the App Sandbox, from wherever they are: on the first start
they ask for the Quake directory (the one with `id1/pak0.pak`), and remember it for the next.
`sys_forget_sandbox` forgets it, and the next start asks again. From a terminal:

```
softworld.app/Contents/MacOS/softworld +map dm4
softworld-server -basedir ~/Games/Quake -port 27500 +map dm4
```

`-basedir` still names the directory, but the sandbox only lets the applications into the one
chosen (and what is in it). The dedicated server isn't sandboxed. Configure with
`-DSW_SANDBOX=OFF` for applications without the sandbox, which take `-basedir` anywhere.

On Linux the client runs on the GPU the compositor draws with, which reads the frame where the
renderer drew it (an integrated GPU; a GPU of its own memory gets a copy), and tells at start
what the compositor offers for latency and HDR. `vid_info` tells it all again, with how long
frames take from present to the screen. The frames go to the display without the compositor
drawing them again (direct scanout) only in fullscreen and where the compositor lets them; the
console says when they do. On Hyprland, whose settings the client reads and names:

```
render:direct_scanout = 2     # fullscreen games scanned out
general:allow_tearing = true  # vid_vsync 0 tears in fullscreen
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
| `vid_hdr`, `vid_hdr_paperwhite` | HDR output on an HDR display; SDR white's brightness in nits (on macOS 0, the default, is the system's white, and nits are over a white of 100; on Linux 0 is the compositor's) |
| `vid_vsync` | 1 a frame at each refresh (the default); 0 doesn't wait for the display |
| `vid_fullscreen`, `-fullscreen`, Alt+Enter | fullscreen: a borderless window on Windows, macOS's own (Option+Enter) on a Mac, the compositor's on Linux |
| `vid_info`, `-gpu n` | Linux: the GPU, the presentation, the compositor's protocols, direct scanout and latency; the n'th GPU instead of the compositor's |
| `r_profile 1`, `r_profile_show` | time a frame takes, by stage |
| `cl_maxfps` | frame rate cap; 0 is none but the display's |
| `demo_speed`, `pause` | MVD playback speed, and pause |
| `demo_jump [+\|-][m:]s` | seek in an MVD |
| `track [name]`, jump, attack | in an MVD or QTV: follow a player, the next one; attack flies the camera and gives it back |
| `demo_itemtimers`, `demo_itemrings` | KTX's item announcements, as a list and as rings on the floor |
| `memstats` | memory by use |

## License

GPL version 2, as id released it; see `gnu.txt`. id's original notes are in `docs/original`.
