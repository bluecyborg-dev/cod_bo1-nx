# bo1-nx

Call of Duty: Black Ops (Xbox 360) on Nintendo Switch homebrew, by static recompilation with
[ReXGlue](https://github.com/rexglue/rexglue-sdk), following the design of
[nfsmw-nx](https://github.com/StevensND/nfsmw-nx) and [masseffect-nx](https://github.com/nebadasSwifty/masseffect-nx).

**Status: bring-up.** The campaign/Zombies executable (`default.xex`) recompiles and links into an NRO. The game is
drawn by ReXGlue's Xenos GPU emulation, which is expected to be very slow on the Switch (1-5 FPS in the other ports).
This build exists to find out how far the game boots. No native renderer, no performance work yet.

Background, findings and the plan: [PORTING_RESEARCH.md](PORTING_RESEARCH.md).

No game data and no code generated from the game is in this repository. You need your own disc image.

## Supported executable

`default.xex` SHA-256 `6027bd4fa4edc750fc60f544ef51feca766290f8a2785e91ebdda6368f281851` (title 41560855, media
0AEB2159, base version, no Title Update). The addresses in `app/overrides.toml` are only valid for that file.

## Build (macOS, Apple silicon)

Needs: Docker, CMake ≥ 3.25, Ninja, Python 3, Apple clang (Xcode command line tools).

```sh
# 1. The game's executable, from your disc image
python3 tools/extract_iso.py cod-bo.iso --xex-only -o assets

# 2. The code generator for this Mac (also fetches the SDK's third-party sources into sdk/thirdparty)
tools/build_host.sh

# 3. Translate default.xex to C++ (app/generated/default, ~90 MB, a few seconds)
tools/codegen.sh

# 4. The NVK Vulkan driver (Docker, ~20-60 min, once): out/mesa-sdk
SRC=$PWD/out/mesa-switch-src OUT=$PWD/out/mesa-sdk mesa/build_mesa_docker.sh

# 5. The NRO (Docker image devkitpro/devkita64): out/nx/bo1-nx.nro (+ out/nx/bo1, the ELF with symbols)
JOBS=6 tools/build_nro.sh
```

## Install and test

```sh
# SD card mounted on the Mac (or a folder you copy to the SD card root afterwards)
tools/make_sd.sh /Volumes/<SD card> cod-bo.iso
```

That gives:

```text
sdmc:/switch/bo1-nx/
  bo1-nx.nro
  bo1.toml          settings
  game_root/        the files of your disc (7.3 GB)
```

Launch it with **full memory**: hold **R** while starting an installed game (title takeover), or use a forwarder
with a **39-bit address space**. The Album (applet mode) does not give enough memory.

After a run, the logs are in `sdmc:/switch/bo1-nx/logs/` (crashes: `logs/rex/rex_crash.log`). Crash addresses
are resolved with `aarch64-none-elf-addr2line -f -C -i -e out/nx/bo1 <address>` against the ELF of the **same**
build.

## Layout

| Path | Contents |
|---|---|
| `app/` | The port: manifest, analysis overrides, app class, settings, CMake project |
| `sdk/` | ReXGlue SDK v0.10.0 with the Horizon (Switch) layer, from masseffect-nx (itself from nfsmw-nx), BSD-3 |
| `mesa/` | NVK build script and patch (masseffect-nx, MIT) |
| `tools/` | Host build, codegen, NRO build, SD layout, ISO extraction, Switch CMake toolchain |
| `refs/` | Reference repositories (not tracked; see PORTING_RESEARCH.md §10) |
