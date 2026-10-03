# SoftWorld

QuakeWorld with a software renderer, grown from id Software's 1999 source release into a
current Windows, macOS and Linux program, and a page in a browser: a client that hosts its own
server, a dedicated server, and the protocol extensions today's servers and clients speak.

- **Renderer:** software only, drawing 32-bit HDR pixels. The render size is 320×200 times a
  whole number, presented through Direct3D 11 on Windows, Metal 4 on macOS and Vulkan on
  Linux (Wayland or X11), with HDR output where supported, and WebGL 2 in a browser. The
  renderer draws straight into memory the GPU reads, or on a GPU of its own memory the GPU copies
  from: the CPU copies nothing (in a browser, a copy a frame).
  Colored lighting (`.lit`, BSPX), BSP2 maps, translucency, skyboxes, fog and TGA textures
  from the map's worldspawn and files, and AVX-512, AVX2 and NEON kernels.
- **Network:** the FTE, MVD1 and ZQuake extensions (float coordinates, 2048 entities, 4096
  models, chunked downloads, …), mvdsv's player movement and its `pm_` keys. WebSocket next
  to UDP: servers take browsers' clients on TCP at their port, as FTE's do. WebRTC to FTE's
  servers, through their brokers.
- **Demos:** QWD and MVD playback, MVD seeking (`demo_jump`), QTV (`qtvplay`), item timers.
- **QuakeC:** a hardened VM with FTE's opcodes and builtins, multiprogs and threads; FTE's
  client-side QuakeC (CSQC), enough for KTX's weapon prediction.

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
| `msvc-v3` | MSVC | x86-64-v3 (AVX2) |
| `msvc-generic` | MSVC | baseline x86-64 |
| `msvc-x86` | MSVC | 32-bit x86, scalar kernels |
| `msvc-asan` | MSVC, AddressSanitizer | x86-64-v4, Debug only |

`SW_ARCH` (`x86-64-v4`, `x86-64-v3` or `generic`) chooses the instruction set: the v4 builds use
AVX-512 and refuse to start on a CPU without it, the v3 builds AVX2 (Intel's since Haswell, AMD's
since Zen), for CPUs without AVX-512. `msvc-x86` builds 32-bit programs, for 32-bit Windows 10:
configure it from a developer prompt for x86 (`vcvarsamd64_x86.bat`, the x64 to x86 cross tools),
or it says so. Its programs have 4 GB of address space on 64-bit Windows, 2 GB on 32-bit, and
draw a little slower than the 64-bit ones. Builds treat warnings as errors
(`SW_WARNINGS_AS_ERRORS`). The build presets build Release; `msvc-v4-debug` and
`clangcl-v4-debug` build Debug. The `msvc-v4-maps` and `msvc-v3-maps` test presets load every
map under the directory the `SW_BASEDIR` environment variable names, and are skipped without it.
`cpu_supported` tells whether the CPU runs the build. CI builds everything and runs the tests
where it does, the maps test on the shareware's.

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
for the window) or an X11 desktop, a GPU with Vulkan 1.3, and:

- clang with lld (the presets' compiler and linker), or GCC; CMake 3.28 or later and Ninja.
- Vulkan's headers and loader, glslang (which compiles the shader), and the development files of
  wayland-client, wayland-protocols 1.41 or later, xkbcommon, PipeWire, Xlib, XInput 2 and
  libXss. On Arch:
  `pacman -S cmake ninja clang lld vulkan-headers vulkan-icd-loader glslang wayland
  wayland-protocols libxkbcommon libpipewire libx11 libxi libxss`.
- Optionally fteqcc, as on Windows.

```
cmake --preset linux-v4
cmake --build --preset linux-v4
ctest --preset linux-v4
```

| Preset | Instructions |
|---|---|
| `linux-v4` | x86-64-v4 (AVX-512) |
| `linux-v3` | x86-64-v3 (AVX2) |
| `linux-generic` | baseline x86-64 or arm64, scalar kernels |

`-debug` build presets build Debug, and `-maps` test presets load the maps as on Windows. The
tests include `test_present_vulkan`, which draws the shader with Vulkan and compares it with what
screenshots make (skipped without a GPU).

Both backends are built by default. Use `-DSW_WAYLAND=OFF` for X11 only or
`-DSW_X11=OFF` for Wayland only.

### Web

The client in a browser, built with Emscripten (6.0 or later) on Windows, macOS or Linux, with
CMake 3.28 or later and Ninja. Install and activate emsdk once (`emsdk install latest`,
`emsdk activate latest`), and in each shell take its environment (`emsdk_env.ps1` on Windows,
`source emsdk_env.sh` elsewhere):

```
..\emsdk\emsdk_env.ps1
cmake --preset web
cmake --build --preset web
ctest --preset web
```

`web-debug` builds Debug. The tests run in node, Emscripten's, and with Chrome `test_present_webgl`
and `test_sound_web` in it, headless (emrun): the shader drawn with WebGL 2 and compared with what
screenshots make, and sound played through its AudioWorklet. The build is WebAssembly with the
scalar kernels, and worker threads in the page as on the systems (`r_threads`).

## Programs

The executables land in `build/<preset>/<config>/`; on macOS the two with a client are
applications (`softworld.app`), whose programs run from a terminal too
(`softworld.app/Contents/MacOS/softworld`).

- **`softworld`:** the client, and a server of its own for `map`.
- **`softworld-client`:** the client without a server.
- **`softworld-server`:** the dedicated server, in a console.

The two with a server carry its game inside them: a game directory without a `qwprogs.dat`
runs the built-in one. The web build makes the two with a client as pages:
`softworld.html` (with its `.js` and `.wasm`), `softworld-fs.js` and `softworld-sound.js`.

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

On Windows `softworld.exe` and `softworld-client.exe` run in an AppContainer, Windows' sandbox,
in Release builds (Debug builds run without it, so a debugger sees the game). The program
started is a launcher that starts itself again inside the sandbox as the game; ending the
launcher ends the game. Without `-basedir`, the first start asks for the Quake directory and
the next ones remember it; `sys_forget_sandbox` forgets it, and the next start asks again.
A directory `-basedir` names is let in as it is, without asking. Letting the sandbox into a
directory gives it a place in the permissions of everything in it, and in the places its links
lead to: some seconds for a large directory, once. A file moved (not copied) into it from
elsewhere on the drive keeps its own permissions, and the game says it can't open it; forgetting
the directory and choosing it again lets the sandbox into all of it again.

The first start also asks whether to let the sandbox reach this computer's own addresses (a
server or QTV proxy on 127.0.0.1, or a client here joining the game's server) and players in
through the firewall, which takes an administrator once (a UAC prompt). Without it the sandboxed
game reaches only other computers. `-DSW_SANDBOX=OFF` builds the programs without the sandbox.

On Linux, Wayland is the default in a Wayland session, with X11 as a fallback. Use
`-window-backend x11` or `-window-backend wayland` to select a backend explicitly.
X11 uses SDR output.

On Wayland the client runs on the GPU the compositor draws with, which reads the frame where the
renderer drew it (an integrated GPU; a GPU of its own memory gets a copy), and tells at start
what the compositor offers for latency and HDR. `vid_info` tells it all again, with how long
frames take from present to the screen. The frames go to the display without the compositor
drawing them again (direct scanout) only in fullscreen and where the compositor lets them; the
console says when they do. On Hyprland, whose settings the client reads and names:

```
render:direct_scanout = 2     # fullscreen games scanned out
general:allow_tearing = true  # vid_vsync 0 tears in fullscreen
```

In the console, Tab completes a command or variable as far as the candidates agree, and Tab
again lists them and goes through them (Shift+Tab back). What completing would add shows
faded after the line; Right or End takes it. The line edits as bash's does: Ctrl+A and Ctrl+E
the start and end, Ctrl+B and Ctrl+F a character back and forward, Ctrl+U, Ctrl+K and Ctrl+W
cut to the start, to the end and the word before, Ctrl+Y puts back what was cut, Ctrl+V
pastes. `apropos text` lists the variables and commands with the text in their name or
description, and a variable's name alone tells what it does, its values and its default.

Worth knowing:

| | |
|---|---|
| `-scale n`, `vid_scale` | render at 320×200 times n (1, the default, is 320×200 itself); 0 picks the largest that fits the window |
| `vid_widescreen`, `vid_crt` | wider view (hor+); CRT pixel aspect |
| `r_lightmode` | 1 linear light in RGB, brighter than white where it is (the default); 0 lighting as Quake had it |
| `r_fullbright_scale`, `r_dlight_scale` | in `r_lightmode 1`, fullbright colors no darker than their color times this (1.5), on walls and models and where light doesn't reach: particles (fire too), sprites, liquids, the sky; dynamic lights' light on surfaces times `r_dlight_scale` |
| `r_externaltextures` | TGA files in `textures/<map>/` or `textures/` in place of the map's textures, truecolor (1, the default); walls take them in `r_lightmode 1` |
| `r_skybox` | a skybox in place of the sky's texture: `<name>rt.tga` and the other five faces in `env/` or `gfx/env/`; empty (the default) for the one the map's worldspawn names |
| `r_fog`, `r_fog_usemap`, `r_skyfog` | fog: the map's, from its worldspawn's `fog` key (`r_fog_usemap 0` leaves it out), with `r_fog`'s over it, as FTE's `fog` command takes it (`"density red green blue"`; 0 is no fog); how far the sky takes the fog's color |
| `r_threads` | threads drawing the view, each walking the world in a band of it; 0 (the default) one a core, at most 8 |
| `r_lerpframes`, `r_lerpmuzzlehack` | models' animation frames blend into each other (1, the default), as in ezQuake; the view model's muzzle flash appears at once rather than blending in from behind the view |
| `gamma`, `vid_contrast` | the view's gamma and contrast; the HUD keeps its own |
| `vid_hdr`, `vid_hdr_paperwhite` | HDR output on an HDR display; SDR white's brightness in nits, 0 (the default) the system's: Windows' SDR content brightness, the compositor's on Linux, the system's white on macOS (where nits are over a white of 100). What is brighter than white goes up to the display's peak, which the console tells at start; on Windows a color profile assigned to the display can change the peak it reports |
| `vid_vsync` | 1 a frame at each refresh; 0 (the default) doesn't wait for the display |
| `vid_fullscreen`, `-fullscreen`, Alt+Enter | fullscreen: a borderless window on Windows, macOS's own (Option+Enter) on a Mac, the compositor's on Linux |
| `vid_info`, `-gpu n` | Linux: GPU and presentation details. Use the n'th GPU |
| `r_profile 1`, `r_profile_show` | time a frame takes, by stage |
| `cl_maxfps` | frame rate cap; 0 is none but the display's |
| `sv_websocket` | the server takes browsers' clients over WebSocket, on TCP at its port (1, the default) |
| `cl_idlefps` | frame rate cap while the window isn't the focus, 50 by default; 0 is `cl_maxfps`'s |
| `cl_truelightning` | how far the lightning beam of the player whose view you see (yours, or the one a demo or spectating follows) turns toward the view, hiding its lag; 1, the default, all the way |
| `demo_speed`, `pause` | MVD playback speed, and pause |
| `demo_jump [+\|-][m:]s` | seek in an MVD |
| `track [name]`, jump, attack | in an MVD or QTV: follow a player, the next one; attack flies the camera and gives it back |
| `demo_itemtimers`, `demo_itemrings` | KTX's item announcements, as a list and as rings on the floor |
| `f_version`, `f_system`, `f_modified` | answered in chat as ezQuake answers them; `f_modified` also as a command, and `allow_f_system 0` answers `f_system` with "disabled" |
| `memstats` | memory by use |

### In a browser

The page needs the game's files from the site that serves it, and cross-origin isolation (the
COOP and COEP headers) for its shared memory. `serve.py` serves a build and a Quake directory
on this machine:

```
python src\platform\web\serve.py build\web\Release C:\quake
```

and the page is at `http://localhost:8000/`. The address's query is the command line:
`softworld.html?+map dm4`, `softworld.html?-scale 3 +connect qw.example.com`. Browsers allow
the shared memory over http on localhost alone; from elsewhere the page must come over https.
Another server needs the same headers, and `manifest.json`, which lists the files (each path,
size and modification time).

The game's files come as the game reads them, a piece at a time, and the browser keeps the
pieces for 30 days or until the file changes on the site. What the game writes (`config.cfg`,
written when the page hides, as a closed tab never quits; demos, screenshots, downloads) is
kept in the browser and read over the site's files at the next start; clearing the site's
data forgets it. Sound and the mouse wait for a click or a key: the browser's rule. The mouse
moves raw where the browser has it (Chrome), and `vid_vsync 0` shows frames as soon as they
are drawn where it lets a page (Chrome too); `vid_info` tells what it has.

A browser has no UDP: the page connects to servers over WebSocket, or WebRTC (`rtc://`, in
WebRTC below). `connect host[:port]` is `ws://host:27500`, or `wss://` from an https page;
`connect wss://quake.example.com/qw` names the URL. SoftWorld's servers (`sv_websocket 1`,
the default) and FTE's take browsers on TCP at their port; mvdsv doesn't. For wss:// put a
TLS proxy in front, which may say who its client is
(`X-Forwarded-For`, believed from the server's machine alone), as Caddy does:

```
caddy reverse-proxy --from quake.example.com --to 127.0.0.1:27500
```

QTV comes over WebSocket too, through a bridge in front of the relay:
`websockify 27600 127.0.0.1:27599`, then `qtvplay 1@ws://host:27600`. TCP delays what comes
after a packet lost until it comes again, which UDP doesn't. The page's own server takes only
its own client.

On Windows the sandbox's firewall rule lets TCP in with UDP since the WebSocket port came; a
rule made before lets UDP alone in, until `softworld -sandbox-network`, run once as an
administrator, makes it again.

### WebRTC

FTE's servers also take clients over WebRTC: a broker (FTE's master, or the server's own)
introduces the client to the server and passes their offers, and the packets then go between
them over a data channel, unordered and never sent again, as UDP's. `connect rtc://broker/room`,
or `rtcs://` for a broker over TLS; the broker's port is 27950 unless the address names one.
The room is the name the server took at the broker, or `udp/ip:port` for a server the broker
knows by its address (FTE's master).

FTE's servers send a packet over WebRTC whole, and a big one is lost on the way: the client
asks for them in pieces under 1384 bytes (FTE's fragmentation, which demos record put
together). `net_rtc_debug` tells what a connection does (2 with libdatachannel's log), and
`net_rtc_ignorecert 1` takes a broker's certificate unchecked, to test with one that has run
out.

The native programs do WebRTC with libdatachannel over mbedTLS, which configuring fetches
(with git) and builds with them, once a build directory; `-DSW_WEBRTC=OFF` leaves them out.
The page does it with the browser's own, which checks the broker's certificate itself; an
https page reaches brokers only by `rtcs://`.

## QuakeC

QuakeC runs on a VM of SoftWorld's own, a C translation of
[qcvm-rs](https://github.com/dsvensson/qcvm-rs), made for untrusted progs: every progs format
and opcode `fteqcc` writes, about 200 of FTE's builtins, several progs in one VM (`addprogs`),
QuakeC threads (`sleep`, `fork`), autocvars, and hard limits, so that a QuakeC error is an error
with a backtrace, never a crash. [docs/qcvm](docs/qcvm) describes it.

The server runs `qwprogs.dat` with FTE's server builtins and offers a `csprogs.dat`
(`sv_csqc_progname`) in the serverinfo as FTE does. The client runs a server's csprogs as FTE's
client does:
- it downloads the csprogs into `csprogsvers` (`cl_download_csprogs`), checks it, and loads it
  with `csaddon.dat` where cheats apply;
- it tells the server CSQC runs, and takes its entities, events and stats (256, as floats and
  strings too);
- sounds and temp entities go to CSQC first, as do the commands before they are sent;
- CSQC draws the view: the scene builtins, a gun of its own, lights, trails and beams.

This is enough for KTX's weapon prediction on mvdsv: the gun, its sounds, projectiles and the
lightning beam show at once, and the server's echo of each sound is dropped. The csprogs'
autocvars become cvars, so its settings (`cl_predict_projectiles`, …) can be changed.
`cl_nocsqc 1` keeps CSQC off, and `csqc_builtins` lists the builtins a csprogs calls that the
client lacks. Traces see the world alone, not entities.

## License

GPL version 2, as id released it; see `gnu.txt`. id's original notes are in `docs/original`.
