# How bo1-nx builds its Switch NRO

As of 2026-10-16.

## Overview

The NRO is built entirely inside Docker on the Mac: nothing Switch-related is installed on the host. One command,
`tools/build_nro.sh`, compiles the recompiled game, the SDK and the native renderer with devkitA64 and links in our
own Vulkan driver (Mesa NVK) to produce `out/nx/bo1-nx.nro`.

Four stages feed it. The first three run rarely; the last runs for every build.

1. **Host tools**: `tools/build_host.sh` builds the ReXGlue code generator and fetches the SDK's third-party sources.
2. **Code generation**: `tools/codegen.sh` translates the game's own `default.xex` into C++ under `app/generated/`
   (never committed).
3. **Graphics driver**: `mesa/build_mesa_docker.sh` builds NVK for Horizon into `out/mesa-sdk-direct/` (about an
   hour, once).
4. **NRO**: `tools/build_nro.sh` configures and builds with CMake + Ninja in the `devkitpro/devkita64` image, links
   everything and runs `nacptool` + `elf2nro`.

```
 Generated game code ----------+
 (codegen.sh from default.xex) |
                               |          +-------------------------+       bo1-nx.nro
 NVK driver -------------------+--------> |   tools/build_nro.sh    | ----> (sent with nxlink_run.sh)
 (build_mesa_docker.sh, once)  |          | devkita64 image, Docker |
                               |          | CMake + Ninja, out/nx   | ----> bo1 (ELF with symbols)
 SDK and app sources ----------+          +-------------------------+       (archived per build)
 (sdk/, app/src/, in git)
```

The generated code and the driver change rarely; the sources change every build, and only `build_nro.sh` reruns.

The result is tested over Wi-Fi with `tools/nxlink_run.sh`, and the unstripped ELF of every build that goes to the
console is archived for reading crash logs.

## Prerequisites and one-time setup

The Mac needs only Docker Desktop, git and Python 3. Everything else comes in two Docker images, both arm64 on Apple
Silicon, so nothing runs under emulation.

| Image | Contents | Where it comes from |
| --- | --- | --- |
| `devkitpro/devkita64:latest` | devkitA64 GCC, libnx, switch portlibs (expat, zlib, zstd), `nacptool`, `elf2nro` | pulled from Docker Hub by the first build |
| `devkitpro-mesa-rust` | the above plus Rust nightly, meson and bindgen for the driver | built locally by the Mesa script from upstream's `Docker.rust` |

One-time steps, in order:

1. `python3 tools/fetch_thirdparty.py`: the SDK's third-party sources into `sdk/thirdparty/` (fmt, spdlog, glslang
   and the rest; `tools/build_host.sh` also runs it when they are missing). glslang is compiled into the NRO by the
   normal build; nothing is installed for it.
2. `tools/build_host.sh`: the code generator, a native Mac program.
3. `tools/codegen.sh`: runs the generator on your own `default.xex` and writes `app/generated/` (then post-processes
   it with `tools/codegen_post.py`). Rerun only when the generator or its configuration changes.
4. `mesa/build_mesa_docker.sh`: the driver (next section).

The game's files, `app/generated/` and everything derived from the game are gitignored and never committed: the
repository is public.

## The Vulkan driver (Mesa NVK)

The Switch has no Vulkan driver homebrew can use, so the NRO links its own: one static archive, `libvulkan.a`,
holding NVK, the NAK shader compiler and its Rust runtime. `mesa/build_mesa_docker.sh` builds it once into
`out/mesa-sdk-direct/opt/devkitpro/portlibs/switch/lib/libvulkan.a`; details are in `mesa/README.md`.

What the script does:

1. Clones [danfromtico/mesa-switch](https://github.com/danfromtico/mesa-switch) at the pinned commit `d4a00ea0ab3`
   (Mesa 26.2.3) into `../mesa-switch-src`, next to the repository.
2. Applies `mesa/mesa-switch-masseffect.patch` and `mesa/mesa-switch-p8-direct-draws.patch`. A stamp file holds their
   hash, so a changed patch resets the tree to the pin and re-applies both; build folders stay.
3. Sets `-Doptimization=2` in upstream's `build-switch.sh`. -O2 measured +5 % draw throughput on the console over
   upstream's -O1.
4. Runs `build-switch.sh`, which builds the `devkitpro-mesa-rust` image, Mesa's host tools and then the Switch cross
   build in `builddir-switch`.
5. Merges `libnak_rs.a` into `libvulkan.a` with an `ar -M` script inside the image, and writes `SOURCE.txt` recording
   exactly what was built.

```
OUT=$PWD/out/mesa-sdk-direct mesa/build_mesa_docker.sh                 # full build
OUT=$PWD/out/mesa-sdk-direct mesa/build_mesa_docker.sh --incremental   # after editing the Mesa source
P8_DIRECT_DRAWS=0 OUT=... mesa/build_mesa_docker.sh                    # without the direct-draws patch
```

The first full build takes about an hour or more and roughly 15 GB of disk. Rebuild only when the pin, a patch or the
optimization level changes; after a rebuild, the NRO relinks on its next build.

## Building the NRO: tools/build_nro.sh

Every build is one command, `tools/build_nro.sh`, which leaves the NRO at `out/nx/bo1-nx.nro` and the unstripped ELF
at `out/nx/bo1`.

| Variable | Default | Meaning |
| --- | --- | --- |
| `MESA_SDK` | `out/mesa-sdk-direct` | driver folder from the previous section |
| `NRO_OUT` | `out/nx` | persistent build folder, relative to the repository |
| `JOBS` | 4 | parallel compile jobs; each needs 1-2 GB of Docker memory |
| `PLAIN_MEMORY` | `ON` | `BO1_GUEST_PLAIN_MEMORY`, passed explicitly so an older build folder follows it |
| `CMAKE_EXTRA` | empty | more `-D` options |
| `IMAGE` | `devkitpro/devkita64:latest` | toolchain image |

Step by step:

1. Checks that Docker answers, that the driver archive and `app/generated/` exist, and that `sdk/thirdparty` is
   complete.
2. Mounts the repository into the container at **the same absolute path** as on the Mac (`-v "$ROOT:$ROOT"`). Paths in
   CMake caches and ninja files stay valid on both sides, so the build folder is reused between runs. A driver folder
   outside the repository is mounted read-only too.
3. Configures and builds:

   ```
   docker run --rm -v "$ROOT:$ROOT" -w "$ROOT" \
     -e JOBS -e ROOT -e SDK_DIR -e MESA_SDK -e NRO_OUT -e CMAKE_EXTRA -e PLAIN_MEMORY \
     devkitpro/devkita64:latest bash -euo pipefail -c '
       cmake -S app -B "$NRO_OUT" -G Ninja -DCMAKE_BUILD_TYPE=Release \
         -DCMAKE_TOOLCHAIN_FILE="$ROOT/tools/switch/cmake/switch-devkitA64.cmake" \
         -DDEVKITPRO=/opt/devkitpro -DREXSDK_DIR="$SDK_DIR" \
         -DREXGLUE_SWITCH_NVK_SDK="$MESA_SDK/opt/devkitpro/portlibs/switch" \
         -DBO1_GUEST_PLAIN_MEMORY="$PLAIN_MEMORY" $CMAKE_EXTRA
       cmake --build "$NRO_OUT" -j "$JOBS"
       /opt/devkitpro/devkitA64/bin/aarch64-none-elf-nm "$NRO_OUT/bo1" | grep "^0000000000000000 T _start$"'
   ```

4. The last line fails the build unless `_start` sits at address 0: if crt0 is not first in `.text`, the NRO does not
   boot.
5. Copies `bo1.nro` to `bo1-nx.nro`, the name the SD card and the Wi-Fi sender use, and prints the ELF's path.

## Linking details

The link is set up in `sdk/cmake/rexglue_switch.cmake`. Four things there are easy to get wrong when porting it.

- **Driver libraries, listed twice.** The NVK target links `vulkan expat zstd z nx vulkan expat zstd z nx stdc++ m`
  with `-pthread`, searching the driver's `lib/`, `$DEVKITPRO/portlibs/switch/lib` and `$DEVKITPRO/libnx/lib`. The
  repeat makes the linker rescan the archives, as Mesa's own config does without `LINK_GROUP`.
- **No `find_package(Vulkan)`.** The Mesa SDK's config requires libelf, which devkitPro does not package.
  `sdk/src/ui/switch_elf_stubs.c` provides the few libelf functions instead; NVK's CUBIN parser that would call them
  never runs on the Switch.
- **Whole-archive for the runtime only.** `rexruntime` and the GPU plugin archive are linked with `--whole-archive`,
  because they override weak libnx symbols (such as `__libnx_exception_handler`), which an archive member would not
  do. `libvulkan.a` links normally.
- **Startup and crash hooks live in the executable.** `rexglue_switch_startup` carries `runtime_switch.cpp` (heap,
  applet type, exception stack) and `switch_crash_hooks.c`, so their `abort`/`exit` replacements reach the link
  before `libc.a`. The `--wrap=` flags next to it belong to the console profiler and are optional.

The NRO itself is two devkitPro tools run by CMake (`rexglue_switch_add_nro`):

```
nacptool --create "Call of Duty: Black Ops" bo1-nx 0.0.1 bo1.nacp
cp bo1 bo1.stripped.elf && aarch64-none-elf-strip --strip-all bo1.stripped.elf
elf2nro bo1.stripped.elf bo1.nro --nacp=bo1.nacp --icon=extras/icon.jpg   # libnx's default icon if missing
```

Only the stripped copy goes into the NRO; `out/nx/bo1` keeps its symbols for reading crash logs.

## Fast rebuilds, A/B builds and archiving

After the first build, changing one source file costs one compile and one link, because `out/nx` is never deleted and
ninja sees the same paths every run. No ccache is used.

- **Memory, not cores, limits `JOBS`.** Docker Desktop here has about 8 GB (`docker info`). The big generated
  translation units need 1-2 GB per GCC job, so more than 4 jobs risks the container being killed mid-build with an
  unhelpful error. Raise both together in Docker Desktop, Settings, Resources.
- **One folder per variant.** `NRO_OUT=out/nx-fast tools/build_nro.sh` keeps a second configuration alongside the
  main one.
- **Swap the driver without recompiling.** Clone the build folder with APFS copy-on-write (instant, no extra disk
  until files change) and point it at another driver; the link re-runs because the CMake file lists `libvulkan.a` as
  a link dependency.

  ```
  cp -c -R out/nx out/nx-test
  NRO_OUT=out/nx-test MESA_SDK=$PWD/out/mesa-sdk-other tools/build_nro.sh
  ```

- **Archive every build that goes to the console.** Crash logs and the console profiler print offsets into the image,
  which only resolve against the exact ELF that ran:

  ```
  cp -c out/nx/bo1        out/elf-archive/bo1-20261016e.elf
  cp -c out/nx/bo1-nx.nro out/elf-archive/bo1-nx-20261016e.nro
  nm -n -C out/elf-archive/bo1-20261016e.elf > syms.txt   # macOS nm reads ELF
  ```

An archived NRO can be sent again later with `NRO=out/elf-archive/<name>.nro tools/nxlink_run.sh <switch-ip>`.

## Mac notes and troubleshooting

| Symptom | Cause | Fix |
| --- | --- | --- |
| `no NVK driver at .../libvulkan.a` | driver not built, or `MESA_SDK` points elsewhere | run `mesa/build_mesa_docker.sh`, or set `MESA_SDK` |
| `app/generated is missing` | code generation not run | `tools/codegen.sh` |
| build stops with no clear compiler error | container out of memory | lower `JOBS` or give Docker more memory |
| `_start` check fails | crt0 not first in `.text` | the NRO would not boot; check link order and toolchain file |
| the NRO did not pick up a new driver | ninja missed the archive change | delete `out/nx` or touch a source so it relinks |
| very slow build | an amd64 image under emulation | `docker image inspect <image> --format '{{.Architecture}}'` must say `arm64` |

- Disk: the images take about 19 GB (`docker system df`), the Mesa source and build about 15 GB more.
  `docker builder prune` reclaims the build cache; keep `devkitpro-mesa-rust` if you want incremental driver builds.
- Run long Mac-side jobs in the background; a foreground run once hung the terminal.
- Never create threads with `std::thread` in code that runs during early start: it crashed the NRO at launch. Use
  libnx `threadCreate`, as `sdk/src/core/log_nonblocking.cpp` does.
