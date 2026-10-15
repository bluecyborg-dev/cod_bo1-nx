# Porting Call of Duty: Black Ops (Xbox 360) to Nintendo Switch homebrew — research notes

Written 2026-10-13. This file is the knowledge base for the project. It's meant to be read by people and by
other AI models before they touch anything. It covers what 8 existing Xbox 360 → Switch ports do, what carries over
to Black Ops (BO1), what we found out about our disc, and a suggested plan.

Reference repos are cloned (shallow, no submodules) in `refs/`, which git ignores. To re-clone them, see
[§10](#10-reference-repositories-and-links).

---

## 0. TL;DR

- **Every working 360 → Switch port uses the same recipe:** static recompilation of `default.xex` to C++ with
  **ReXGlue** (PPC → C++ → AArch64 with devkitA64/libnx), packed as an **NRO**, plus a **native Vulkan renderer** on
  **NVK (Mesa) linked into the NRO** via [danfromtico/mesa-switch](https://github.com/danfromtico/mesa-switch).
  Emulating the Xenos GPU (the SDK's default backend) always ran at **1-5 FPS** on Switch, so every port replaced it.
- **NFS:MW (nfsmw-nx) is the reference design.** Project 8, The Simpsons Game and Torchlight build on it directly,
  and Mass Effect follows the same design. Its `sdk/` (the Horizon layer for ReXGlue) is BSD-3 and meant to be
  reused. Its `docs/` folder is the best porting manual that exists. Read `refs/nfsmw-nx/docs/porting-another-game.md`
  first.
- **There's already a BO1 PC recompilation: `bo1-recomp`** (ReXGlue, Title Update #11, SP/Zombies + MP, 60 FPS on PC).
  It's the "stage 1" that nfsmw-nx got from NFSMW-Recompiled. The original repo (ItsManueh) has been deleted. A mirror
  survives at **[LucioXerus/bo1-recomp](https://github.com/LucioXerus/bo1-recomp)**, cloned in `refs/bo1-recomp`. Its
  SDK fork (`ItsManueh/rexglue-sdk`, branch `bo1`) is **gone**, and no surviving fork has it.
- **Our disc:** XGD2, title ID `41560855`, media ID `0AEB2159`. It has **two executables**: `default.xex`
  (campaign + Zombies, 34 MB image, entry `0x824DBDB8`) and `default_mp.xex` (multiplayer, 39.8 MB image, entry
  `0x82536390`). Both are base version, with no Title Update applied. Videos are **Bink 1 (`BIKi`) 1280x720 @30**.
  Data is in IW fastfiles (`IWff0100`, version `0x1D9`) and `pak2` image/sound packs.
- **The hard parts for BO1, beyond what NFSMW faced:** (1) a CoD engine with six hardware threads of busy-wait
  polling, running on 3 Switch cores at 1 GHz. (2) The engine writes PM4 and constants straight into command
  buffers and reuses precompiled command buffers. A PM4-reading renderer (the nfsmw-nx design) fits this well.
  (3) Shaders live inside compressed fastfiles. (4) Bink cutscenes: the recompiled decoder will be too slow, so
  replace it with FFmpeg (the same pattern as NFSMW WMV3 and Simpsons VP6). (5) The image is 2.5× bigger than
  NFSMW's, so build RAM and time go up.
- **Recommended starting point:** the nfsmw-nx SDK (Horizon layer) + bo1-recomp's manifests, overrides and engine
  hooks + Project 8's simpler native renderer (runtime shader translation) for first frames. Later, NFSMW's renderer
  techniques for speed.

---

## 1. The common architecture of every port

```
default.xex (PPC, big endian)
   │  rexglue codegen  (runs on the host PC; manifest TOML + hand-written overrides)
   ▼
generated C++ (one function per guest function: sub_XXXXXXXX(ctx, base)), never committed
   │  post-generation passes (direct calls, register fixes, ...)
   ▼
devkitA64 GCC (AArch64) + libnx + ReXGlue runtime (Horizon layer) + libvulkan.a (NVK/NAK)
   ▼
game.nro   (+ unstripped ELF kept for addr2line on crash logs)
```

At runtime, ReXGlue provides an HLE Xbox 360 kernel (xboxkrnl/xam), guest memory, threads, file system (VFS over
`sdmc:/switch/<game>/game_root`), XAudio/XMA (via FFmpeg), input (HID) and presentation. The game's own statically
linked **Direct3D 9-ish XDK layer** still runs and writes a **PM4 command ring**. A native renderer reads that ring
on its own thread ("ring thread") and records equivalent Vulkan commands. EDRAM isn't emulated: render targets become
plain Vulkan images and resolves become copies.

Distribution model used by every port: the repo contains **no game code or data**. The user's own disc is the input.
NRO builds are shipped, and a browser installer page (WASM) builds the package and the shader library from the
user's ISO locally.

Launching: **title takeover** (hold R on a game) or a **forwarder** with a **39-bit address space**. Album/applet
mode only gets ~400 MB, which isn't enough.

---

## 2. Per-port findings

### 2.1 Need for Speed: Most Wanted — `nfsmw-nx` (StevensND) ★ reference

- ReXGlue v0.10.0 fork in `sdk/`. Native Vulkan renderer in `app/src/nfsmw_nativo_*` (~50k lines of game-side code).
  Mesa patch `mesa/mesa-switch-nfsmw.patch` against mesa-switch `1a8c1a66d6f`. Shader library (`.nfsp`) built ahead
  of time with XenosRecomp + DXC.
- **30-35 FPS at stock clocks** (32-35 in races; 21-22 in the heaviest spot with ~3,000 draws/frame).
- Built on **Windows** (PowerShell, MSYS2 for Mesa). Code is commented in Spanish, docs are in English.
- Docs worth reading in full: `docs/porting-another-game.md`, `platform-notes.md`, `native-renderer.md`,
  `toolchain.md`, `shaders.md`, `mesa.md`, `measuring.md`, `performance-history.md`, `audio-and-video.md`,
  `editions.md`, `console-overlays.md`.

**Performance history (race FPS):** emulated GPU 1-3 → first native renderer 16.1 → GPU work 60→33 ms gives 26.0 →
frame pacing fixed → CPU per draw cut + official 460.8 MHz handheld GPU profile gives 30.3 → direct calls + LTO + PGO +
function ordering gives ~34 → native rewrites of hot game functions gives 35.4.

**Key techniques (all reusable):**
- *Codegen register options:* `cr_as_local`, `xer_as_local`, `ctr_as_local`, `non_volatile_as_local`. The last one
  removed `__savegprlr_*`/`__restgprlr_*` calls, which were ~14 % of the main thread. Split function chunks then need
  `share_registers` (a codegen patch, `emit_call_sharing_registers`), and `tools/lee_antes.py` finds registers that
  are read before they're written.
- *Direct calls* (`tools/llamadas_directas.py`): every `sub_X` is a **weak** alias, and GCC never inlines through a
  weak alias. Rewrite calls to `__imp__sub_X` when the function isn't hooked (79,612 of 83,077 calls). **Re-run it
  after every codegen**, because codegen silently undoes it. A function counts as hooked if its address appears
  anywhere in the sources.
- *`[rexcrt]`:* map the guest's memcpy/memset/memmove/strncpy to native versions by address.
- *LTO* (`-flto=2`, needs ~16 GB RAM), *PGO* (`-fno-profile-values` is **mandatory** because `tpidr_el0` is 0 on
  Horizon, plus `-fprofile-correction`; never regenerate between the two builds), *function ordering* (the first line
  in `.text` must be `KEEP(*(.crt0))`, and `_start` must be at 0 or the NRO won't boot).
- *xxHash strict-aliasing trap under LTO+PGO:* use `XXH_FORCE_MEMORY_ACCESS 0`.
- *Busy waits:* hook the game's D3D polling loops (`nfsmw_espera_anillo.cpp`, `nfsmw_espera_fotograma.cpp`) so they
  sleep with a timeout, then call the original.
- *Native replacements of hot game functions* behind a **guard**: run native and original side by side for the
  first 50k-200k calls (then 1 in 4096), compare every output, use the original's result, and disable the native
  version forever on any difference. Match FMA shape for bit-exactness.
- *Renderer:* 3 work slots (frames in flight), 64 MB upload buffer per slot, resolve without copy (swap images),
  two-level texture cache with content hashes (re-check less often, down to every 32 frames), NEON untiling, texture
  sub-allocation from 16-32 MB blocks, a pipeline cache plus a list of known pipelines precompiled at boot
  (113/113 ready in 0.3 s), `static_assert(has_unique_object_representations_v)` on hashed keys, no atomics for
  single-thread counters (A57 has no LSE), sampled timers only.
- *Shaders:* scan containers by signature (`10 2A 0E 00` pixel, `10 2A 0E 01` vertex). Older games use the 2005
  container header and need a converter. XenosRecomp → HLSL → DXC → SPIR-V, keyed by microcode hash. **Vertex
  shaders can't be identified by microcode at draw time** (D3D patches them), so hook the shader-creation functions.
  Big wins: constants through a dynamic UBO (+18-23 % FPS), merged predicated blocks (1806 → 641 `if(p0)`), drop
  `max(a,a)` copies, 1/size uniforms instead of size queries, depth-only pipelines without a pixel shader. Fixes:
  `PRED_SET_INV` case, normals read as float, screen-space draws (`PA_CL_VTE_CNTL=0x400`), depth-only draws.
- *Mesa/NAK:* ZCULL re-enabled (−2 ms), texture latency 32→200 cycles, `peephole_select` limit 8, the `FADD32I`
  dropped-saturate bug ("black sea"). **Raise the NAK revision on every compiler change**, or old shaders get reused
  from the cache.
- *Audio:* a 187.5 Hz pump, a small queue, the audio server at priority 0x2D, XMA decoding on a worker instead of on
  the kick, an FFmpeg MDCT table cache, and a timeout+retry on a lost audio event.
- *Video:* the game's recompiled **WMV3 decoder took 98 % of a core**. Feed the same bytes to FFmpeg instead
  (7-15 ms/frame). FFmpeg must be an LGPL build.

### 2.2 Mass Effect — `masseffect-nx` (nebadasSwifty)

- UE3. Same family as nfsmw-nx, **built on macOS (Apple Silicon) with Docker** (`devkitpro/devkita64`). That
  matches our machine. Scripts: `tools/build_host.sh`, `tools/codegen.sh`, `tools/build_nro.sh`,
  `mesa/build_mesa_docker.sh` (~1 h, ~15 GB).
- ~25 FPS at 960x544 internal resolution. CPU-bound (main thread ~30 ms).
- `tools/extract_iso.py`: a pure-Python XDVDFS extractor that prints the XEX header info. **We used it on our ISO**
  (see §4).
- Emulates EDRAM with one Vulkan image per format plus transfers ("mode 4"). Making transfers rare and cheap was
  the biggest effort.
- **Measured no gain from LTO, PGO, function ordering or args-in-registers** (unlike NFSMW). `-Os` was worse.
  What did help: register locals plus a "diet" (`elide_lr_stores`, `dform_disp_split`, `fctiwz_inline`,
  `skip_msr`, `reserved_as_local`, `non_argument_as_local`), and `pch_no_volatile.py` (guest loads/stores not
  volatile, +15-20 %).
- **Compiler flags for generated code:** `-ffp-contract=off` (**required for correctness**: GCC otherwise fuses PPC
  fmul+fadd into FMA and results drift), `-fno-strict-aliasing`, `-fno-math-errno -fno-trapping-math`.
- Platform facts it adds: commit guest memory in **2 MB granules** (64 KB ran out of kernel memory blocks, error
  2001-0103). There's **no file mmap**, so the **ISO can't be used directly** and must be extracted. Never read
  `cntvct_el0` (it faults); use `armGetSystemTick`. **The kernel loses q0-q7/q16-q31 when a thread in the exception
  handler is preempted**, which is the deep reason page-protection GPU emulation is unsafe on Horizon. The official
  CPU boost throttles the GPU to 76.8 MHz (unusable). The heap is capped at 1024 MB. There's a 0x800-entry GPFIFO
  limit (reserve the tail or you get "device lost"). A 4.5 GB guest window.
- `docs/do-not-break.md` and `optimization-paths.md` record every idea tried. Copy that discipline.
- Xenia's per-game patch files (MSAA/tiling bytes) are a cheap source of game-specific knobs. Check xenia-canary
  `game-patches` for `41560855`.

### 2.3 Skate 3 — `skate3recomp-nx` (NaGaa95)

- A Switch build of the PC Skate 3 recomp (rexglue fork based on SDK **0.8.0**, submodule
  `mchughalex/rexglue-skate3`). One commit, "switch port", with `__SWITCH__` branches in the app.
- A **different renderer philosophy**: a *data-driven scene renderer* that reads engine entities from guest memory
  and runs hand-ported material shaders (`src/native/shaders/*.hlsl`). That's a huge game-specific effort. Not
  recommended for BO1.
- Switch specifics: memory profile from the process's available memory, caches capped, MSAA/HDR/SSAO off by
  default, the game at `sdmc:/switch/skate3/game`.

### 2.4 The Simpsons Game — `the-simpsons-game-nx` (R-drg)

- Lineage: TheSimpsonsGameRecomp (PC) → Switch layer + renderer from project8-sw → nfsmw-nx design.
- 30 FPS needs an **overclock** (CPU 1785 / GPU 768). Stock gives about 19 FPS.
- Cutscenes are VP6. The recompiled decoder took a whole core, so FFmpeg VP6 replaces it (`movie_decoder.cpp`).
- Four costliest full-screen effects use hand-written replacement shaders from the PC port.

### 2.5 Sonic '06 — `MarathonRecomp-NX` (ChanseyIsTheBest) and Sonic Unleashed — `UnleashedRecomp-NX` (NaGaa95)

- **XenonRecomp/XenosRecomp family** (hedge-dev), not ReXGlue. These ports *replace the game's D3D calls* with a
  high-level renderer (plume, an RHI) and precompile all shaders. That works because those engines call D3D
  functions throughout. **The CoD engine writes PM4 directly, so this approach fits BO1 poorly** (bo1-recomp's own
  analysis says the same).
- Marathon NX: 55-60 FPS CPU-limited spot at 1020 MHz after its perf rounds. `docs/SWITCH-PERFORMANCE.md` and
  `SWITCH-OPTIMISATIONS.md` catalogue ~100 knobs (UBO constants, trimmed constant uploads, depth-only without PS,
  alpha-test early-out, lazy resolves, persistent pipeline cache, `-ffixed-x16` for the exception resume path, plain
  guest memory with loop barriers).
- **Memory-ordering finding:** UnleashedRecomp-NX patches XenonRecomp so that `sync`/`lwsync`/`eieio` emit real
  barriers (`PPC_SYNC`/`PPC_LWSYNC`). The upstream no-ops are fine on x86 (TSO) but **wrong on ARM's weak memory
  model**. **ReXGlue (including nfsmw-nx's fork) still emits them as no-ops** (`sdk/src/codegen/builders/system.cpp`,
  "x86 has strong ordering"). For a heavily multithreaded engine like CoD, this is a latent source of rare races.
  Consider emitting `dmb ish` for `sync`/`lwsync`, at least as an experiment.
- Unleashed NX also uses a Horizon address-arbiter wait/notify (`os/switch_atomic_wait.h`: `svcWaitForAddress`)
  instead of yield spinning. Yield never runs a lower-priority thread on the same core.

### 2.6 Tony Hawk's Project 8 — `project8-sw` (R-drg)

- Built from Project8Recomp (PC) + nfsmw-nx's renderer design. **The most useful "how we adapted nfsmw-nx" docs:**
  `docs/NFSMW_NX_REUSE_ANALYSIS.md` classifies every nfsmw-nx SDK file as generic (A/B), NFSMW-only (C), conflicting
  (D) or licence-sensitive (E). `docs/SWITCH_PORT_ANALYSIS.md` is a feasibility audit. Also has `AGENTS.md`.
- SDK changes are shipped as a **patch series against upstream ReXGlue v0.10.0** (`patches/rexglue-sdk/series`,
  16 patches). Mesa changes are patches too (`patches/mesa-switch/`, pinned `d4a00ea0ab3`). That's a cleaner way to
  maintain than vendoring the SDK. **Recommended for us.**
- **A simpler renderer than NFSMW:** shaders are translated **at runtime** by the SDK's own Xenos→SPIR-V translator
  on a worker thread, pipelines are created asynchronously (a draw is skipped until it's ready), and the driver
  pipeline cache lives on SD. No offline shader library was needed. Good for first frames.
- 30 FPS (with GPU 768 MHz via sys-clk). Pacing cap `thps_min_frame_vblanks=2`, plus `thps_pace_before_present`.
- Bring-up bugs seen on hardware (useful for us): no loader-owned `vkEnumerateInstanceLayerProperties` (report 0
  layers), link `libnvk.a` whole-archive, `dirent.d_type == DT_UNKNOWN` on SD (classify entries via `stat`), and an
  SD transfer left `default.xex` at 0 bytes (check the hash at startup).
- **Bink cutscenes freeze** (first frame, then a green/magenta frozen image): Bink paces video by audio, and the
  audio clock never advances. **BO1 uses Bink too**, so expect the same issue.
- Busy-wait replacements that return exactly what the original loop would: `guest_swap_wait.h`, `guest_job_wait.h`.

### 2.7 Torchlight — `torchlight-nx` (liartes)

- Ogre3D engine, XBLA. Built with the nfsmw-nx SDK + patches, Docker, on Linux. 30-35 FPS town at stock clocks.
- **Renderer intercepts at the engine level** (Ogre's D3D9RenderSystem methods), not PM4. The game generates HLSL
  at runtime, which is compiled in-process with glslang. That's engine-specific and not applicable to CoD.
- `CLAUDE.md` (in French) is a working log kept by an AI-assisted workflow. It's a good model for keeping session
  state. Lessons: `std::thread` throws `system_error` on the console (use `pthread_create` with an explicit stack
  size), `std::ifstream` throws on read errors, `std::filesystem` lists folders badly, and an open file can't be
  renamed. `0xCE01` was kernel memory blocks exhausted (fixed by 2 MB commit granules). Use a forwarder rather than
  title override: in title override you inherit the replaced title's system resources, which may be none.

### 2.8 Summary table

| Port | Recompiler | Renderer approach | Shaders | FPS / clocks | Build host |
|---|---|---|---|---|---|
| NFS:MW | ReXGlue 0.10 fork | PM4 ring → Vulkan | offline library (XenosRecomp) | 30-35 stock | Windows |
| Mass Effect | ReXGlue fork | PM4 ring → Vulkan + EDRAM "mode 4" | offline library | ~25 stock | **macOS + Docker** |
| Project 8 | ReXGlue 0.10 + patches | PM4 ring → Vulkan (NFSMW design) | **runtime** SDK translator | 30 (GPU 768) | Linux + Docker |
| Simpsons | ReXGlue + patches | from Project 8 | runtime | 30 (OC), 19 stock | Linux + Docker |
| Torchlight | nfsmw SDK + patches | engine-level (Ogre) intercept | runtime HLSL → glslang | 30-47 stock | Linux + Docker |
| Skate 3 | ReXGlue 0.8 fork | data-driven scene renderer | hand-ported | — | — |
| Unleashed / Marathon | XenonRecomp | D3D call replacement (plume) | offline XenosRecomp | 55-60 (Marathon) | Windows/MSYS2 |

---

## 3. Horizon / Switch platform facts (consolidated)

**Hardware at stock:** 3 usable Cortex-A57 cores at **1020 MHz** (core mask 0x7). GPU GM20B Maxwell, 256 cores,
**256 KB L2**, 768 MHz docked. In handheld a homebrew app starts at 307.2 MHz, and the official config
**`0x92220008`** gives 460.8 MHz GPU with 1331.2 MHz memory (not an overclock; set via `apmSetPerformanceConfiguration`,
which is asynchronous, so poll ~1.5 s). `0x92220007` raises memory to 1600 by mistake. No FP16 at 2× in NAK on
pre-Turing. Erista heats up earlier than Mariko. ~3.2 GB for the application.

**Memory**
- Horizon limits mapping count and memory blocks (`0xCE01` = 2001-0103 `ResourceExhausted`, not OOM). The Xbox 360
  sees physical memory through up to 5 mirror views. Commit lazily (exception handler), map a view on first touch,
  in **2 MB** granules.
- `svcSetProcessMemoryPermission` is refused on `svcMapProcessMemory` memory, so "protecting" a page means
  unmapping it. Heap memory can't be a `svcMapProcessMemory` source (`0xD401`), so detour via
  `svcMapProcessCodeMemory`.
- `svcGetInfo(UsedMemorySize)` reads ~the whole heap from the start under hbloader. Measure free memory inside the
  heap or via `VK_EXT_memory_budget`.
- NvMap handle count is limited by `__nx_nv_transfermem_size` (8 MB ≈ 4,000 handles, error `0x235C`). It's a weak
  symbol you can raise. Sub-allocate textures.
- No file mmap and no dlopen (static GPU plugin and static ICD).

**Exceptions:** libnx has one handler, one stack and one dump. Use a pool of 8 stacks/dumps claimed with
`ldaxr/stlxr`. Vector registers can be lost under preemption inside the handler. No SEH →
`generate_exception_handlers` is off (games relying on guest SEH need care; bo1-recomp notes BO1 has SEH blocks).
`-ffixed-x16` if the resume path branches through x16.

**Threads:** **only priority 0x3B is time-sliced**. Every other priority is cooperative (a busy thread starves
equal/lower priorities on its core, even with other cores idle). Put guest threads at 0x3B and host service threads
above (0x2B audio/XMA, 0x2C presentation, 0x2D ring thread). If a thread starves, *lower the hog's priority*. A
preferred core isn't a pin (an exclusive mask is). `std::thread::detach()` and `std::thread` creation from guest
threads can throw; use persistent pthreads. Decide sleep/wake under a lock, and give every wait a timeout plus a log
line. Thread names get a ` (F80000xx)` suffix, so match by prefix.

**NVK on Horizon:** every fence check is an ioctl and every `vkQueueSubmit` is IPC (keep submits per frame low).
GPU allocations are slow (~1.9 ms per dedicated texture). Timestamps tick at **1.627 ns**, not 1 ns. One queue,
exactly 3 swapchain images, FIFO/IMMEDIATE only (IMMEDIATE doesn't tear). Pipeline creation costs ~59 ms uncached.
Turn off Mesa's SD disk cache when you keep your own `VkPipelineCache`. ZCULL is compiled out in stock mesa-switch.
A shader writing depth disables early-Z/ZCULL (<1 FPS in ME).

**Frame pacing:** the compositor shows frames in steps of 16.67 ms. A steady 30 (swap interval 2, frames ≤ ~31 ms)
feels better than a higher, uneven average. Always report the % of frames > 33/50 ms.

**Overlays:** SaltyNX never injects into homebrew. Write its shared block yourself (template in
`refs/nfsmw-nx/extras/saltynx-template`) for FPS in Status Monitor and ReverseNX-RT.

**Crashes:** `logs/rex/rex_crash.log` plus the Atmosphère report → `aarch64-none-elf-addr2line -f -C -i -e <unstripped ELF>`
(subtract 4 from return addresses). Keep the unstripped ELF of every build that leaves your machine.

**Measuring:** at stock clocks only (overclocked numbers misled every project), A/B alternation inside one session
behind a runtime setting, a no-op control to measure noise, CPU time per thread rather than wall time, and read the
code behind each counter.

---

## 4. Our disc (facts gathered so far)

Read-only inspection with `refs/masseffect-nx/tools/extract_iso.py --list` / `--xex-only` (output in the
session scratchpad, not the repo):

| | |
|---|---|
| Image | `cod-bo.iso`, 7,838,695,424 bytes, **XGD2** (game partition at `0x0FD90000`), 98 files, 7.27 GB of content |
| Title ID / Media ID | `41560855` / `0AEB2159` |
| `default.xex` (SP + Zombies) | 8,245,248 B, SHA-256 `6027bd4fa4edc750fc60f544ef51feca766290f8a2785e91ebdda6368f281851`, image base `0x82000000`, **image size 34,013,184 (0x2070000)**, entry `0x824DBDB8`, AES + basic compression, version field `0.0.0.3` as printed by the ME tool |
| `default_mp.xex` (MP) | 8,802,304 B, SHA-256 `b76de77a5b29994579f01d8bfd1f14bc902505dff366ced150851a0c8a7b17f5`, image size 39,780,352, entry `0x82536390` |
| Fastfiles | `*.ff`, magic `IWff0100`, version `0x000001D9`, then `PHEEBs71` (CoD T5 360 format; zlib-compressed zones). Per-level `.ff` of ~100 MB plus `common*.ff`, `code_post_gfx*.ff`, `frontend.ff`, `ui_mp.ff`, `mp_*.ff`, `zombie_*.ff` |
| Packs | `images.pak` (2.35 GB, magic `pak2`, streamed textures), `images_low.pak`, `snd.all.pak` (713 MB), `snd.english.pak` (393 MB) |
| Videos | `*.bik`, **Bink 1 `BIKi`, 1280x720 @ 30 fps** (e.g. `treyarch.bik` 587 frames, `cuba_load.bik` 1406 frames). Level loading screens are videos (`*_load.bik`). |
| Other | `$SystemUpdate/`, `nxeart` |

Open questions about the disc:
- bo1-recomp's `config/default.toml` says the base `default.xex` is "version 0.0.0.8", while the ME tool prints
  `0.0.0.3`. This may be a different field or a different disc revision/region. Compare function addresses
  (e.g. setjmp `0x825FB540`, longjmp `0x825FB940`, the leaf setter `0x8239BE70`) against our executable before
  reusing that config.
- bo1-recomp targets **Title Update #11** (`0.0.11.8`, SP+MP `.xexp` + patched fastfiles). We don't have TU11.
  Options: get the TU11 package (STFS; `refs/bo1-recomp/tools/stfs.py extract`), or target the base executables
  with `config/default.toml` / `default_mp.toml`, which bo1-recomp ships as the "bootstrap" configs.
- Rendering resolution: bo1-recomp's code says the 3D scene is **960x544** ("the console's 10 MB EDRAM holds no
  more"). Digital Foundry reported **1040x608 2×MSAA** on 360 (960x544 on PS3). Verify from the game's render
  target setup once it runs. Either way it's sub-720p, which is good for the Switch GPU.

---

## 5. What bo1-recomp gives us (`refs/bo1-recomp`)

MIT-licensed port code. Requires ReXGlue v0.10.0 + the missing `bo1` SDK branch (D3D12 fixes, split screen, audio,
SMAA... mostly PC-specific).

- **Manifests:** `bo1_manifest.toml` (TU11 SP), `mp/bo1mp_manifest.toml`, and bootstrap `config/*.base.toml` (base xex).
- **Analysis configs** (`config/default*.toml`), all with evidence comments: setjmp/longjmp addresses (needed
  because `Com_Error` longjmps back to the main loop), vtable-only functions, thunks, tail-jump-only leaves, and SEH
  blocks deliberately excluded. 19,554 SP / 21,820 MP functions reachable.
- **Register options used:** `skip_msr`, `ctr_as_local`, `xer_as_local`, `reserved_as_local`, `cr_as_local`.
  **NOT `non_volatile_as_local`, NOT `skip_lr`**, because setjmp/longjmp and the SEH blocks need r14-r31 and LR in
  ctx. That's a different situation from NFSMW: NFSMW's big ~14 % gain from `non_volatile_as_local` will need
  extra work here (setjmp support or `share_registers` around the Com_Error path).
- **Symbols:** `config/symbols_tu11.toml` / `symbols_mp_tu11.toml`. 1,933 of 19,317 functions are named from string
  tables, command and script builtin tables (`tools/name_functions.py`). TU11 addresses only.
- **Engine hooks** (`src/bo1_engine.cpp`, TU11 addresses), done by redefining the weak `sub_X`:
  `Com_Frame` (SP `0x82315590` / MP `0x82343D60`), `Cbuf_AddText`, `Com_Error`, `Dvar_RegisterBool`, `Dvar_FindVar`,
  the dvar hash table, the dvar_t layout, **job queue "run one" (`0x82533240`) and `WaitForSingleObject`
  (`0x824BE9F8`)**.
  - **Busy waiting is massive:** the two job queue workers call `SwitchToThread` ~55,000×/s each, and other threads
    poll events with a zero timeout ~54,000×/s. The port sleeps 0.1 ms after 2 ms idle and waits up to 1 ms after N
    consecutive zero-timeout polls. On Switch this is mandatory, together with the 0x3B time-slicing rule.
  - Native `FindNameInTable` with a guard (entity spawning was slow).
- **Kernel tweaks** (`src/bo1_kernel.cpp`): `KeGetCurrentProcessType` is called ~800,000×/s, so read the PCR from r13
  directly. Campaign ↔ MP switching (`XamLoaderLaunchTitle("default_mp.xex")`) is handled by relaunching the other
  exe. On Switch that would mean relaunching the other NRO (`envSetNextLoad`) or **linking both executables into one
  NRO** (two code images, about 2× binary size). That's a design decision for later.
- **Frame pacing** (`bo1_framerate.cpp`): under emulation the console's `r_vsync` produced 12/22 ms alternation. The
  port turns `r_vsync` off and paces `Com_Frame` itself. MP must run at a fixed rate.
- **TU handling** (`bo1_tu.cpp`): ReXGlue codegen doesn't apply `.xexp`, but the runtime does. Make a bootstrap build
  and dump the patched XEX (`--bo1_dump_xex`), then recompile that.
- **Performance data on PC (i5-9600K):** emulated GPU thread ≈ 1 core. ~30 % of it is driver, ~30 % guest memory
  uploads (650-800 per frame with write-protection), ~40 % register/state/texture work. **~8,300 draws per frame in
  split screen.** Single player is lower, but expect thousands of draws per frame. NFSMW's heaviest spot (~3,000
  draws) dropped to 21 FPS. **Draw count will be the main CPU problem for the ring thread.**
- **`docs/native-renderer.md`** (roadmap): the XDK D3D is statically linked without symbols. Found so far: the
  `DRAW_INDX_2` packet builders (SP `sub_824912E0`, `sub_824928A0`, `sub_824929F8`, `sub_82499708`). **The CoD engine
  writes registers and constants straight into the command buffer and reuses precompiled command buffers**, so
  D3D-call replacement (Unleashed style) is hard. A **PM4-ring reader (nfsmw-nx style) handles this naturally**,
  because it sees the final stream whatever produced it. Shaders are in fastfiles. MSAA 2x/4x is used. Vertex buffers
  and textures are written directly to GPU-shared memory (no Lock/Unlock to hook).
- **Tools:** `stfs.py`, `xex_extract.py` (needs `pycryptodome`), `ppcdis.py` (capstone), `find_missing_funcs.py`,
  `find_setjmp.py`, `find_string_callers.py`, `classify_candidates.py`, `xref_global.py`, `dump_cvars.py`,
  `sampler/`.

**Related CoD-engine recomps** (sources of engine knowledge): `officialmelon/bo2-recompiled` (BO2, T6, also SP+MP
xex, "playable with issues"), `goshavindtburg/NX1recomp` (CoD: Future Warfare), `zenit2893-cmyk/project-1944`
(CoD 3). PC BO1 modding knowledge (Plutonium T5, BO1-Reimagined GSC) and the 360 `Coreyx86/T5GSCLoader-Xbox-360`
plugin (360 function addresses) could help name functions.

---

## 6. BO1-specific risks and how other ports handled the equivalent

| Risk | Why | Mitigation seen elsewhere |
|---|---|---|
| CPU budget (3 × 1 GHz A57 vs the 360's 3 cores / 6 HW threads at 3.2 GHz) | CoD runs ~6 threads, a 60 Hz sim, and thousands of draws | Kill busy waits (bo1-recomp hooks), 0x3B time-slicing, register locals, direct calls, no-volatile guest memory, native hot functions behind guards, target **30 FPS** SP (MP sim rate needs thought: the game ticks at a fixed rate) |
| Draw count / ring thread | 1-3k+ draws/frame SP, 8.3k in split screen | NFSMW per-draw work (state hash caches, dedup uploads, set-4 diffs, Mesa draw-path patch); Project8's register-hash state cache; drop split screen on Switch at first |
| Precompiled command buffers and direct PM4 writes | engine bypasses D3D | ring reader (nfsmw/Project8/ME design) handles indirect buffers (`INDIRECT_BUFFER` packets) |
| Shaders in compressed fastfiles | the container scan needs decompressed zones | Phase 1: runtime translation (Project 8 design). Phase 2: decompress `.ff` (zlib, IWff 0x1D9) offline/in installer → scan for `10 2A 0E 0x` → XenosRecomp library |
| Bink 1 cutscenes and **loading-screen videos** | the recompiled Bink decoder will eat a core, and Project8's Bink froze on audio clocking | replace with FFmpeg `bink`/`binkaudio` (LGPL build), as NFSMW did with WMV3 and Simpsons with VP6; keep the game's data callbacks |
| setjmp/longjmp in `Com_Error`, SEH blocks | block `non_volatile_as_local`/`skip_lr` | declare setjmp/longjmp (done in bo1-recomp); later investigate ME's note on unsupported longjmp |
| Memory | 512 MB guest physical + streaming `images.pak` + GPU textures in ~3.2 GB | lazy commit with 2 MB granules, texture cache cap + eviction (NFSMW 512 MB cap), NvMap handle limit raise |
| `sync`/`lwsync` no-ops on ARM | multithreaded job system | experiment with `dmb ish` emission (Unleashed NX does it) |
| Two executables | SP/Zombies vs MP | Start with SP (`default.xex`); MP later as a second NRO with `envSetNextLoad` |
| Large code (34 + 40 MB images) | long builds, 16 GB RAM on this Mac | Docker with enough memory, LTO off at first (ME found no gain anyway), `-O2` for generated code if RAM-limited |
| Floating-point exactness | AI/physics determinism | `-ffp-contract=off` on generated code (ME), FMA-shape care in native replacements |

---

## 7. Suggested plan (in the order the other ports found worked)

0. **Repo hygiene** (done): git repo, `.gitignore` for `*.iso`, `*.xex`, `game/`, `extracted/`, `refs/`. Never commit game
   data or generated code. Consider Project 8's `check_no_game_content.sh` approach.
1. **Extract the disc** into `game/` (needs ~7.3 GB free; the disk currently has ~15 GB free):
   `python3 refs/masseffect-nx/tools/extract_iso.py cod-bo.iso -o game`.
2. **Decide the TU question:** get TU11 (then reuse bo1-recomp's TU11 configs, symbols and hooks as-is) or stay on
   base (use `config/default.toml`, `default_mp.toml`, and re-derive hook addresses with `find_string_callers.py`).
   Using TU11 saves a lot of address re-derivation.
3. **Host codegen:** build `rexglue` for macOS. Base it on the nfsmw-nx `sdk/` (it has the Switch layer plus the
   `share_registers` codegen work), following masseffect-nx's `tools/build_host.sh`, which is macOS-tested. Run
   codegen on `default.xex` with bo1-recomp's manifest and config. Fix missing functions until codegen validates.
   `xdzleo/xenon-jumptables` helps with unresolved jump tables.
4. **(Optional but recommended) PC/macOS bring-up** with the SDK's stock Xenos backend (MoltenVK on macOS, which Skate 3
   and ME have done), to fix translation issues where debugging is easy. bo1-recomp has already proven the game
   runs under ReXGlue on PC.
5. **Switch bring-up:** devkitA64 (installed at `/opt/devkitpro`) or the Docker image `devkitpro/devkita64`. Build
   mesa-switch NVK with the nfsmw patch (Docker, ~1 h, ~15 GB disk, which is tight right now). Link, get to boot,
   logs, the intro video (it plays at startup; `com_introPlayed` can skip it), and the "Press Start" menu. The stock
   Xenos backend will run at 1-5 FPS, which is enough to confirm the logic runs.
6. **Busy waits and thread priorities** (bo1-recomp hooks ported to Horizon: sleep with libnx primitives / the
   address arbiter).
7. **Native renderer:** start from project8-sw's `src/game/src/native/` (adapted from nfsmw, GPL-3, simpler, runtime
   shader translation). First trace the game's D3D layer (`nfsmw_d3d_trace.cpp` pattern) to confirm addresses.
   Then grow it toward NFSMW's techniques.
8. **Bink → FFmpeg**, audio priorities, XMA on a worker.
9. **Measure at stock clocks** (NFSMW `measuring.md` method). Then the toolchain optimizations, offline shader
   library, and Mesa patches.
10. Installer page / packaging, following nfsmw-nx-installer (browser, WASM, builds from the user's ISO).

**Licensing note:** the nfsmw-nx app code is GPL-3.0. Its SDK changes are BSD-3, and its Mesa/XenosRecomp changes are
MIT. project8-sw's native renderer is GPL-3 (adapted from nfsmw). bo1-recomp is MIT. Reusing renderer code makes
our project GPL-3. FFmpeg must stay an LGPL build, linked so it can be relinked.

---

## 8. Host environment notes (this machine)

- Apple M4, 10 cores, **16 GB RAM**. Docker is at `/usr/local/bin/docker`, devkitPro is at `/opt/devkitpro`
  (devkitA64 present), plus Homebrew cmake/ninja, Apple clang, and python3.
- ReXGlue requires real LLVM clang (not Apple clang) for the host tool: `brew install llvm` (Skate 3's macOS
  presets expect `/opt/homebrew/opt/llvm`).
- Disk: ~15 GB free after the user cleaned up. The extracted game needs 7.3 GB, generated C++ needs several GB, and a
  Mesa build needs ~15 GB. Free more space before step 5.
- 16 GB RAM: LTO links need ~16 GB, so keep LTO off and use limited `JOBS` (ME used 6 jobs at 1-2 GB each in Docker).

---

## 8b. Build log: first bring-up NRO (2026-10-13)

What was done and what was learned. See README.md for the commands.

- **Base:** `sdk/` = masseffect-nx's SDK (a superset of nfsmw-nx's Horizon layer: 2 MB commit granules, vector
  register save in the exception handler, GPFIFO fix, non-blocking log), `mesa/` = masseffect-nx's Docker build +
  patch (mesa-switch `d4a00ea0ab3`), host tools from masseffect-nx. All of it built on this Mac without changes,
  apart from two fixes below.
- **Our `default.xex` is not bo1-recomp's base build.** Decrypted with `refs/bo1-recomp/tools/xex_extract.py`
  (`out/default_image.bin`, needs `pycryptodome` + `capstone` in `out/venv`). Build date 2010-09-28, version
  0.0.0.3. bo1-recomp's base config is version 0.0.0.8. Same code but shifted, so its addresses can't be reused,
  only re-derived:
  - setjmp `0x825FB3A0`, longjmp `0x825FB7A0` (theirs `0x825FB540`/`0x825FB940`, −0x1A0)
  - leaf setter `0x8239C0D0` (theirs `0x8239BE70`). That was the **only** codegen validation error.
  - 28 vtable-only functions/thunks (find_missing_funcs + classify_candidates), the same set as theirs. 17 SEH
    funclets dropped on purpose.
  - Result: **18,912 functions**, 86 C++ files, ~88 MB of generated code, codegen in ~7 s on the M4.
  - bo1-recomp's engine-hook addresses (Com_Frame, job queue, WaitForSingleObject...) are TU11. For our build
    they must be found again with `find_string_callers.py`.
- **Register options:** `skip_msr`, `ctr/xer/reserved/cr_as_local` (as bo1-recomp). No `non_volatile_as_local`.
- **Fix 1:** masseffect-nx's repo lacks its modified `sdk/thirdparty/FFmpeg/config.h` (its `.gitignore` whitelists
  it but the file was never committed), so the fetched upstream file errors with `#error "no config"` on Switch.
  Fixed by recreating it from nfsmw-nx (an `__SWITCH__` branch → `config_switch_aarch64.h`, plus the WMV3
  components header).
- **Fix 2:** the local devkitPro has no `switch-zlib/zstd/expat` portlibs, so the NRO is built in the
  `devkitpro/devkita64` Docker image (GCC 15.2), which has them.
- **Timings on the M4 / 16 GB:** host generator ~3 min, Mesa NVK ~20 min (on a fresh Docker), NRO ~10 min with 6
  jobs in Docker (8 GB VM). The NRO is 45 MB (ELF 64.5 MB). `_start` is at 0.
- **SD card (development card, 256 GB FAT32): it silently drops writes.** After the first copy, 6 blocks of
  128 KB (4 × 32 KB clusters) in the NRO and in 3 game files still held their previous contents (deleted
  nfsmw-nx logs). The filesystem itself was consistent (`fsck_msdos -n`). Some blocks never accepted a write
  even after rewriting 5 times with `F_FULLFSYNC`. **On macOS a read right after a write comes from the page
  cache (even with `F_NOCACHE`), so the only reliable check is unmount, mount, compare.** Fix: the damaged files
  were moved to `switch/bo1-nx/bad_blocks_do_not_delete/` (so the bad clusters stay allocated) and fresh copies
  were written and verified. `tools/make_sd.sh` now remounts and verifies every byte with `tools/verify_sd.py`.
  If the game shows unexplained data errors, suspect the card first.
- **First hardware test (2026-10-13): black screen, then "The software was closed because an error occurred".**
  - Run 1 was launched from the Album (applet mode: every XMA context "Couldn't allocate", then `std::bad_alloc`
    creating the Xenos command processor). Use title takeover (hold R) or a forwarder.
  - Runs 2-3 (title takeover): runtime, NVK device, VFS and **guest code all worked** for ~0.4 s (interrupt
    callback, files, `XCustomSetAction`, rumble). Then the guest main thread ended ("Execution complete"). On the
    way out the SDK crashed: `MnkInputDriver::~MnkInputDriver` → `DetachFromWindow` →
    `CallInUIThreadSynchronous` throws `std::system_error` on Horizon. Fixed in `sdk/src/input/input_system.cpp`:
    no MnK driver on Switch.
  - **The same build on macOS** (`out/mac`, SDK host build with MoltenVK built from source; needs
    `-DREXGLUE_ROOT=$PWD/sdk` for the ICD copy and the ImGui include fix in `app/CMakeLists.txt`) **keeps running**:
    audio 6 ch 48 kHz, DLC query, network init (recoverable `Com_Error` drops: "Failed to recvfrom() socket").
    So the translation is sound and the Switch failure is platform-specific: it diverges right after the rumble
    call, where the Mac goes on to audio initialization.
  - `Com_Error` = `0x82332180` in our build (hooked in `app/src/bo1_engine.cpp` to log messages).
    `OnGuestThreadExit` logs the main thread's exit status/r3/lr.
  - Run the Mac build: `out/mac/bo1 --game_data_root=$PWD/game` (logs in `out/mac/logs/`).
- **Second hardware test: the game quit cleanly (no error screen) with `Com_Error` fatal "WARNING: Couldn't
  allocate IP port"**. BO1 opens its UDP socket (port 3074) at start-up and treats failure as fatal. Three SDK
  network bugs, all fixed in `sdk/` (marked `bo1-nx:`):
  1. `xam_net.cpp`: nothing called libnx `socketInitialize` for the game, so every guest `socket()` failed on
     Horizon. Now `EnsureSwitchSockets()` runs once on the first XNetStartup/WSAStartup/socket.
  2. `xsocket.h`: the native `sockaddr`/`sockaddr_in` used the Linux layout (`uint16_t family`). Horizon (libnx,
     FreeBSD-derived) and macOS use `uint8_t len; uint8_t family`, so bind() saw family 0. Now the BSD layout
     is used on Mac and Switch (`REX_XSOCKADDR_BSD_LAYOUT`).
  3. `xam_net.cpp`: non-Windows paths reported WSAGetLastError = 0, so a non-blocking `recvfrom` with nothing
     waiting looked like a broken socket ("Failed to recvfrom() socket, error UNKNOWN ERROR", an ERR_DROP).
     Now errno is mapped to Winsock codes (EWOULDBLOCK → 10035). The Mac drops disappeared.
- **Third hardware test:** sockets initialized; then `bind(port 1000)` → errno 13 (EACCES) → `Com_Error` fatal
  "Failed to bind socket, error 31". Xbox 360 titles bind UDP 1000 for system link; Horizon refuses ports < 1024
  to homebrew (macOS allows them). Fix in `sdk/src/system/xsocket.cpp`: on EACCES for a privileged port, retry
  on port + 50000 (local only).
- **Fourth hardware test: the Treyarch intro plays, then "freezes".** The game didn't hang: it presented ~7 FPS
  for 20 s and loaded `frontend.ff` meanwhile. The profiler showed guest threads "Backend" (97.6%) and "Server"
  (92.4%) spinning, i.e. 2 of the 3 cores. The intro's Bink decoding starved (and Bink paces video by audio,
  which was silent). Found by sampling the macOS build (`sample <pid> 5`), fixed in `app/src/bo1_engine.cpp`:
  - Backend: `sub_8238FC18(condition, help)` = wait until `condition()` while helping the job queue, with no
    sleep. Hooked its job check `sub_825443F0` (pending-jobs test): 64+ empty polls in a tight loop sleep 100 µs.
  - Server: `WaitForSingleObjectEx` (`sub_824DE0B8`) polled with timeout 0. After 256 timeouts on the same handle,
    wait up to 1 ms (bo1-recomp's rule).
  - On the Mac afterwards: Backend 641/786 samples sleeping, Server 785/786 in a timed wait.
- **Fifth hardware test: the intro plays longer, then freezes again.** CPU total fell from 240% to 69% (Backend
  12%), so spinning is fixed. Now every thread mostly waits:
  - GPU emulation is the frame-rate limit: "fences waited 49 (918.7 ms)" per second, 52 EDRAM render-target
    transfers per frame → 7.3 FPS. This is the known reason every port replaced the Xenos emulation with a
    native renderer.
  - **The game's audio mix is silent on the Switch** (0/470 audout buffers with sound, peak 0.000), while on the
    Mac it has sound (new meter in `sdk/src/audio/audio_system.cpp`: "[audio] game mix": peak 0.43-0.50, ~100% of
    frames). Bink paces video by its audio clock, so silent audio is the likely cause of the video freeze. The
    game's mixer (Sound Mix thread: `sub_8245EAA8` → `sub_82463C20` → `sub_8249F718`) runs on both platforms.
    Next: the meter on the Switch tells whether the game mixes silence or the output driver loses it.
- **Sixth hardware test (audio meter every 2 s):** the user reports sound plays and freezes with the picture at
  ~2 s. Meter: 258/383 frames with sound in the first window, then exact silence for 80 s while the game keeps
  presenting ~9.5 FPS at 39% CPU. (The Switch driver's "audout buffers with non-zero PCM" stat is wrong: it read
  0 while sound was audible.) During the freeze every thread waits. The Bink code runs as a job on the "Server"
  thread (job queue worker: `sub_823DEE68` → `sub_82545D98`). Cinematic job: `sub_825A02B8` → `sub_8259FAB0` →
  `sub_8259EF58` (decode) → Bink `sub_826156B0`... Correction (trace on the Switch): the cinematic job keeps running ~8-10/s (one per game frame) and `sub_8259EF58` keeps returning 1 ("playing"), so the player is called but doesn't advance. That is consistent with the underrun pause below.
  - Not the slow GPU: the Mac with `REX_DEBUG_SLOW_SWAP_MS=100` (debug aid added to
    `sdk/src/graphics/command_processor.cpp`) plays the video through.
  - Mac baseline of the cinematic trace (hooks in `bo1_engine.cpp`, logged every second): job and decode ~60
    calls/s (one per frame), decode returns 1.
- **Reference found by the user: [MisaDev4/bo1-zombies-decompiled](https://github.com/MisaDev4/bo1-zombies-decompiled)**
  (sparse clone in `refs/bo1-zombies-decompiled`, `src/gfx_d3d src/binklib src/universal src/qcommon src/win32
  src/sound`). A decompilation of the **PC** (Steam, D3D9, binkw32) zombies engine: readable C++ of the same engine
  with original function names (`R_Cinematic_*`, worker commands, `Sys_*`...). Addresses don't match our 360
  build, but it explains the logic. It's decompiled Activision code: use it as a reading reference only and never
  copy it into this repo.
  - `r_cinematic.cpp` `R_Cinematic_Advance`: Bink keeps a read buffer filled by its background I/O. Below 20%
    full (`percentageFull < 0x14`) it sets `underrun` and **pauses** the video (picture and sound). Below 50% it calls
    `R_Cinematic_SeizeIO` (`Sys_SuspendDiscReads(THREAD_OWNER_CINEMATICS)` + `BinkControlBackgroundIO(bink, 2)`);
    above 95% it gives the disc back (`R_Cinematic_RelinquishIO`). That matches the Switch symptom.
  - In our 360 build: cinematic glob at `0x83F5E9F0` (playbackFlags +772, binkIOSize +780); `sub_8259EEE0(const
    BINKREALTIME*)` = the percentage computation (+44 ReadBufferSize, +48 ReadBufferUsed); `sub_8259EF58` ≈
    `R_Cinematic_Advance` (returns 1 while playing); job `sub_825A02B8` = the `_UpdateFrameWorkerCmd`.
  - Mac trace: buffer 12.98 MB, starts at 8%, fills, drains at 30 frames/s, refills to 100% below 50% (frame ~240).
- **Seventh hardware test (buffer trace): not an underrun.** The video stopped at frame 68 with the Bink buffer 84%
  full, while `R_Cinematic_Advance` kept being called. From frame 9 to 68 the video ran at ~17 FPS (Bink decoding on
  the 1 GHz cores is slower than real time) while the sound played in real time.
  - **Cause: a Bink audio-clock deadlock.** With a soundtrack, `BinkWait` follows the audio. When decoding falls
    behind, the decoded audio runs out, the audio clock stops, `BinkWait` says "wait" forever, nothing is decoded
    and no audio is submitted again.
  - **Reproduced on the Mac** with `BO1_DEBUG_CINE_JOB_SLEEP_MS=60 BO1_DEBUG_CINE_ADVANCE_SLEEP_MS=60` (debug aids in
    `bo1_engine.cpp`): frozen at frame 220, silent. Only slowing the GPU or the job rate did NOT reproduce it
    (Bink's catch-up loop keeps up while each call is fast).
  - Functions (call order matches the PC `R_Cinematic_Advance`): `BinkGetRealtime` 826150F0, `BinkPause`
    82614FB8, **`BinkWait` 82614E00**, `BinkDoFrame` 82615948 (`li r4,775; b 826156B0`), **`BinkShouldSkip`
    82614DA0**, `BinkNextFrame` 82615950. BINK: +0 width, +4 height, +8 frames, +12 FrameNum, +20/+24 FrameRate/Div.
    The 360 build's underrun threshold is 2% (`cmplwi r3,2`), not the PC's 20%.
  - **Fix: wall-clock fallback** (like the PC engine's timer-based "custom skip logic"). Every time Bink's own clock
    says "go", re-anchor (time, frame). If `BinkWait` says "wait" while the video is behind real time since the anchor,
    answer "go". If `BinkShouldSkip` says "no" while more than one frame behind, answer "catch up". Mac repro: the
    whole intro plays at 30 frames/s, sound comes back, the next video starts; healthy runs show `forced 0`.
  - Longer term: Bink decoding is too slow on the Switch. Decode Bink 1 natively with FFmpeg (`bink` + `binkaudio`
    decoders), like NFSMW did with WMV3 and the Simpsons with VP6.
- **Eighth hardware test: the intro plays to the end** (Bink fallback confirmed). Then two `Com_Error` drops
  "Failed to recvfrom() socket, error WSAEINVAL" (my errno map's default, so an unmapped errno) and the main
  thread exited 0.3 s later. Fixes:
  - `sdk/src/core/socket_posix.cpp`: Winsock `FIONBIO` (0x8004667E, big-endian guest argument) now goes through
    `fcntl(O_NONBLOCK)` instead of passing the request to `ioctl()`.
  - `sdk/src/kernel/xam/xam_net.cpp`: on the Switch any `recvfrom` failure is reported as WSAEWOULDBLOCK
    (offline only: no Xbox Live, no system link), and the real errno is logged up to 8 times.
- **Ninth hardware test: 78 s in, then a crash at the main menu** (memory access fault at 0x5f inside NVK
  `write_sampled_image_view_desc` ← `nvk_UpdateDescriptorSets` ← SDK `VulkanCommandProcessor::UpdateBindings`),
  right after 5 pipelines failed: NVK "SPIR-V parsing FAILED: Source and destination types of SpvOpStore do not
  match: int vs. float".
  - **Root cause: a bad merge in masseffect-nx's `spirv_translator.cpp`**: `var_main_tfetch_lod_ =
    var_main_vfetch_bound_ = <int variable>;` followed by a discarded float variable. The texture LOD was stored
    into an int variable. MoltenVK accepts it; Mesa doesn't. Fixed (nfsmw-nx has these lines correct).
  - Found by dumping the translated SPIR-V on the Mac (added to `sdk/src/graphics/vulkan/pipeline_cache.cpp`,
    behind `--dump_shaders=<dir>`; files `*.vulkan.{vert,frag}.spv`) and validating with `spirv-val --target-env
    vulkan1.2 --uniform-buffer-standard-layout --scalar-block-layout` (`brew install spirv-tools`): 181 shaders, 0
    errors after the fix.
  - **macOS build trap:** `out/mac/librexgpu-xenosrd.dylib` is a copy that is only refreshed when the app relinks.
    After changing only GPU plugin code, copy `sdk/out/mac-arm64/librexgpu-xenosrd.dylib` by hand. (The earlier
    `REX_DEBUG_SLOW_SWAP_MS` experiment may have run on a stale plugin.)
  - Network: `recvfrom` errno 14 (EFAULT): Horizon's socket IPC can't access the guest memory window. On the Switch
    `XSocket::Recv/RecvFrom/Send/SendTo` now go through a per-thread heap bounce buffer. Also fixed byte order
    of ports/addresses in `RecvFrom`/`SendTo` and `sin_len` for BSD stacks.
- **Tenth hardware test: no crash. Intro and a second video play (~70 s), then a hang at 0 FPS with CPU 310%.**
  The Mac at the same point starts a third video (the menu background) and goes on.
  - During the hang Backend, JQ Worker 1/2 (~85% each, 0% waiting) and Main all sit in the job queue: "find a job"
    `sub_825450A8` (scans 8 queues, yields via `sub_824CFF58` when empty), reached from wait-and-help loops
    (`sub_8238FC18`, `sub_8238F7F0`). Something they wait for never runs.
  - The "Server" thread (a job worker that also runs the network frame) spent ~40% of its samples **inside
    `recvfrom`** (`sub_8242D9E8` → `sub_823C49F8` → `sub_823C4880` → `sub_823C47F0` → `NetDll_recvfrom`), against
    2 samples on the Mac: the receive blocks on Horizon even after `fcntl(O_NONBLOCK)`.
  - Fix: on the Switch, `XSocket::Recv/RecvFrom` always pass `MSG_DONTWAIT`; calls over 5 ms are logged with the
    socket's O_NONBLOCK state.
- **Wi-Fi testing set up (2026-10-14): `tools/nxlink_run.sh [switch-ip]`.** Uploads the NRO with devkitPro's
  `nxlink` (the user runs **Sphaira**, whose built-in nxlink receiver works) and records the game's log live:
  the SDK's `log_network` setting (`--log_network=<mac-ip>:28771`) streams every log line and stdout/stderr over TCP
  to `nc -l 28771` → `out/net_logs/<time>.log`. Launched this way `argv[0]` isn't the usual path, so the script
  also passes `--game_data_root=sdmc:/switch/bo1-nx/game_root` (and the app now falls back to that folder). The
  profiler (`rex_profile.log`) is still written to the SD card only.
- **Eleventh hardware test (first over Wi-Fi): the game reaches its "Press Start" title screen** (3D front end) after
  a black screen of some seconds, and stays up. It runs at **~1 frame per second** (the menu's cinematic job runs
  ~1/s), with the NVK channel saturated ("channel 2 hit native submission pressure ... retiring oldest accepted
  work"). The previous "hang at 0 FPS" was this extreme slowness. Input seems dead because a frame-polled button
  press is missed at 1 FPS. The Xenos GPU emulation on the 3D scene (1040x2528 depth targets = EDRAM tiling with
  MSAA) is the bottleneck, as every port found.
- **Twelfth hardware test: the menus are navigable at ~1 FPS** (Zombies → Kino der Toten selected). The game crashed on
  the loading screen: "Unhandled guest access violation: read of guest 0x0FFAA58D" in the Database thread while
  loading `common_zombie.ff` (`r3` dest 0x82862624 valid, `r4`/`r30` source 0x0FFAA58D unmapped, LR 0x824CF058, back
  chain 821F8F18 ← 821F9440 ← 8220F834 ← ... ← 8220C9B0). No allocation failure logged. Candidate cause: Mass
  Effect's documented register corruption under the emulated GPU's page-watching faults (Horizon loses
  q0-q7/q16-q31 when a thread is preempted in the exception handler), which they cured only by replacing the
  emulated GPU. Not confirmed. `--cl=+devmap zombie_theater` reaches ExLoadedCommandLine but this build doesn't
  load the map from it (bo1-recomp injects commands through `Cbuf_AddText` at `Com_Frame` instead).
  - **`Cbuf_AddText` = `sub_8232EA30(localClientNum, text)`** (enters critical section 0x2F, the command buffer lock,
    so it's callable from any thread). Debug aid `BO1_DEBUG_EXEC="devmap zombie_theater"` (env, Mac) or
    `--bo1_debug_exec=devmap+zombie_theater --bo1_debug_exec_delay_s=150` (setting, Switch via `EXTRA_ARGS` of
    `tools/nxlink_run.sh`) queues one command from the cinematic job hook. **On the Mac Kino der Toten loads fine**
    (103 MB read, 444 pipelines, no crash), so the Switch crash is platform-specific: register corruption under the
    emulated GPU, bad bytes from the failing SD card, or another Switch memory-layer issue.
  - **Kino crash reproduced on the Switch with the automatic load**: 8 s after `devmap zombie_theater`, only 7.4 MB
    read, same Database-thread loader chain (`821FB060` ← `821FB0FC` ← `821FB27C` ← `8220A410` ← `821F9B54` ←
    `8220C8A4` ← `8220C9B0`), a different garbage address (0x9ACA4D70 via r10+0x20). No warnings before it.
  - Leading suspect: **the SDK's file read caches** (masseffect-nx's `host_path_file.cpp`: per-file read-ahead
    "window" `masseffect_io_window_kb=256`, range cache `masseffect_io_ranges_mb=64`) copy into the guest buffer with a
    plain NEON `memcpy`. A store into a page not yet mapped in that view, or watched by the emulated GPU, faults
    mid-copy, and Horizon can corrupt q registers across that round trip. Mass Effect saw wrong bytes on the console
    from exactly this path in their block cache (`docs/streaming-io.md`, "Incident: wrong data on the console") and
    switched to writing through the shadow alias. Test: `--masseffect_io_window_kb=0 --masseffect_io_ranges_mb=0`.
  - Disabling the read caches did not help: the crash repeated with identical registers (deterministic).
  - **Cause found: bad bytes on the SD card.** `--bo1_debug_hash_files=common_zombie.ff,zombie_theater.ff` (FNV-1a 64
    per MiB, computed by the NRO at startup, compared with `out/hash_ref.txt` from the ISO) showed **MiB 46-48 of
    `common_zombie.ff` differ** on the Switch; everything else matched. These chunks had passed `verify_sd.py` after
    the first copy, so the card degrades after writing (or something overwrote those clusters).
    `tools/repair_sd_file.py <volume> <file>...` quarantines the damaged copy and rewrites + verifies a fresh one.
  - **Twist: the Mac read that same copy correctly** (after a remount; `fsck_msdos -n` clean apart from an unset
    FSInfo free count). So the Switch and the Mac got different bytes from the same file: a card that misreads
    some blocks under the Switch's bus mode, or a FAT difference (e.g. FAT copies/active-FAT flag) only one OS follows.
    `verify_sd.py` on the Mac therefore cannot prove the Switch sees good data; only the on-Switch hash can.
    The copy was moved to quarantine anyway (`repair_sd_file.py --force`; the first rewrite again lost MiB 2, the
    second verified) and the whole game_root re-verified on the Mac (0 bad chunks).
  - **Thirteenth hardware test: Kino der Toten loads, no crash.** On the Switch the new copy hashes right, and the
    quarantined old copy still hashes wrong in exactly the same MiB 46-48 with the same values: a stable,
    location-specific misread that only the Switch sees. **Rule: when the Switch shows a deterministic data error
    that the Mac can't reproduce, hash the files on the Switch (`--bo1_debug_hash_files`, entries may be absolute
    like `/switch/bo1-nx/...`) and move bad copies with `tools/repair_sd_file.py --force`.** Replace the card when
    possible.
  - Under Sphaira's nxlink the socket service is already up (`socketInitialize` → 0xF59): now treated as success.
- This SDK (masseffect-nx's) emits `lwsync`/`sync` as `std::atomic_thread_fence` (seen in generated code), so the
  ARM memory-ordering concern from §2.5 doesn't apply to it.
- Profiler stack sampling on the Switch: create `logs/rex/stacks_profile.flag` next to the NRO.
- Deploying to the bad card: never overwrite in place. Move the old file into `bad_blocks_do_not_delete/`, copy
  fresh, remount, `cmp`, and retry (needed 2 attempts for each NRO so far). Expected first problems, from the other ports: indirect-call targets the static
  analysis can't see ("Call to invalid or unregistered function at guest address X" in the log means adding
  `X = {}` to `[functions]`), the Bink intro video (`treyarch.bik`) decoded by recompiled code (slow), busy-wait
  threads at priority 0x3B, and very low FPS from the Xenos emulation.

---

## 8c. Native renderer bring-up (2026-10-14)

- **Source:** project8-sw's `src/game/src/native/` (GPL-3.0, itself from nfsmw-nx), copied to `app/src/native/` with
  names changed (`thps_*` → `bo1_*`). It has **no game addresses**: it replaces the SDK's graphics system
  (`config.graphics` in `Bo1App::OnPreSetup`) and reads the PM4 ring. Selected with `--bo1_renderer=native`
  (default stays `xenos`). Shaders translated at run time by the SDK's SPIR-V translator, pipelines on a worker.
- **What our SDK (masseffect-nx's) lacked, added with `bo1-nx:` tags:** 6 Vulkan 1.0 functions in
  `device_1_0.inc` (`vkCmdCopyImage`, `vkCmdWriteTimestamp`, `vkGetQueryPoolResults`, `vkCreate/DestroyPipelineCache`,
  `vkGetPipelineCacheData`) and `timestampPeriod`; `RexGmSetReadOnlyMapped` (read-only guest pages stay mapped,
  ported from project8-sw's patch 0009) in `guest_memory_switch.cpp`; on the desktop the `rexgpu-xenos` plugin now
  exports its symbols and the app links it (the renderer uses the translator/register file/draw helpers). In the app:
  a local `SleepUntil`, `bo1_native_ring_core` (their SDK's `switch_cp_core`), `gpu_specialize_texture_signs`
  → always false (our translator lacks it), `XXH_INLINE_ALL`.
- Also from project8-sw: `-fno-tree-slp-vectorize -fno-tree-vectorize` on the recompiled code (GCC paired guest
  context copies into q registers, and Horizon cleared q31 across a page fault: a loading-screen crash for them).
- **Mac (MoltenVK) first run:** works at once. Intro video at 60 swaps/s; 3D front end ~1,650 draws per frame.
  Draws needing a geometry shader (rectangle lists etc., skip reason 5) are skipped on Metal only; NVK has them.
- **Vertex copies were the first cost:** project8-sw copies each vertex fetch constant's *whole* buffer (up to
  16 MB) when its hash changes. CoD's large shared, partly-rewritten buffers made that ~330 MB/frame and 35 ms of
  renderer CPU per frame *on the M4*. Fix (`bo1_native_ranged_vertices`, default on): a `ShaderTranslator`
  subclass walks each vertex shader once and proves every `vfetch_full` reads the untouched `r0.x` (the translator
  stores the guest vertex index there: `clamp((index + VGT_INDX_OFFSET) & 0xFFFFFF, VGT_MIN_VTX_INDX,
  VGT_MAX_VTX_INDX)`); then only `[lo*stride, hi*stride + extent)` is copied and the fetch base is rebased to
  `copy_offset - start`. Index bounds are cached per index buffer by content hash. All 93 front-end vertex shaders
  qualify. Result on the Mac: **~3 MB/frame, renderer CPU 2.7 ms, 60 fps** in the front end.

- **Switch, first native runs (2026-10-15):** menu ~11-12 FPS (renderer CPU ~81 ms/frame, GPU ~52 ms as reported),
  Kino ~4-6 FPS with ~4,000-4,800 draws/frame. Game logic fine; the 3D world was black. Causes, found with the debug
  trace (`--bo1_debug_trace_frame=N` or `--bo1_debug_trace_after_s=S`, first 1000+-draw frame after S s) and resolve
  image dumps (`--bo1_debug_dump_host=<mac>:28772` + `tools/dump_receiver.py`, or `--bo1_debug_capture_dir` on the
  Mac; `tools/rgbx_to_png.py` converts), all fixed in `app/src/native/native_renderer.cpp` / `native_textures.cpp`:
  1. **One image per (EDRAM base, format):** the scene is drawn as `k_8_8_8_8_GAMMA` and resolved as `k_8_8_8_8` from
     the same tiles. Fix: color formats with the same host VkFormat share an image (`CanonicalTargetKey`).
  2. **Xbox D3D clears through a 4x view at half the pitch** (a rectangle-list draw, depth ALWAYS + write, into
     `ms 4x, pitch/2`, same EDRAM tiles as the 2x/1x surface). Fix: such rectangles go to the existing full-pitch
     target with viewport/scissor scaled (`FindClearAlias`). Same for shadow maps.
  3. **Resolves through a 4x view of a 1x surface = free 2x2 downsample** (bloom/exposure chain, `C5@1782 p560` →
     `p280 ms 4x`). Fix: `ResolveDownsampled` blits the 1x image at half size with linear filtering (`vkCmdBlitImage`
     added to the SDK table).
  4. **Resolve exponent bias ignored:** the exposure chain writes `k_16_16_16_16` with RT exp bias 5, resolves with
     +11 and samples with fetch exp_adjust -16 (net 0). Host 16_16_16_16 RTs are SNORM at 1/32 scale, so the copy
     needs bias+5 (the SDK's rule). Fix: `bo1_native_resolve_exp_bias=2` (default) adds the bias to the exp_adjust of
     fetch constants that sample the destination (exact for filtering, no copy shader needed).
  5. **No 3D textures** (project8-sw never needed them). BO1's 32x32x32 color grading table is a 32x1024 color resolve
     sampled as a tiled 3D texture. Fix: `ResolveDestination::Volume`, copied with regions from the guest tiling of
     both (`GetTiledOffset2D`/`3D`): the mapping is a scramble of **16x8 blocks** (256 regions). 3D textures from
     memory (one 512x256x4 `k_8_8_8_8`) are still unsupported.
  6. **One resolve destination per address:** BO1 resolves its shadow depth and then the color table to `1DDD0000`
     every frame, so both 8 MB images were destroyed/recreated twice a frame (the NVK "VA bind ... 0x235C" /
     `nvMapCreate` failures after a few minutes) and the volume never survived. Fix: depth and color destinations
     coexist (`kColorTag`), sampling takes the kind written last.
  7. **Rainbow noise on world surfaces = stale low mips.** Level 0 of streamed textures was right (decoded from raw
     guest memory), level 1+ was high-entropy garbage *in guest memory*. BO1's texture streamer copies the low mips
     into place with a **GPU memcpy**: a point list whose memexport vertex shader `A4827826E2589916` reads 64 bytes
     per vertex (8 × `FMT_16_16_16_16` integer fetches) and exports them as 8 elements of 8 bytes to the stream in c0
     (element index `c0.y + (c255.w*i)*c255.y`, second half `+ c255.z`; c255 = 0,1,4,8). The renderer skipped every
     memexport draw (reason 2). Fix: `EmulateMemcpyExport` does it on the CPU in ring order
     (`bo1_native_memexport_copy`, default on); fetch 8in32 + `.yxwz` and export 8in16 cancel, so it is a byte copy.
     Debug aid: `--bo1_debug_mip_watch` (mip entropy at first sight and later).
  - Also: textures freed by a memory budget (`bo1_native_texture_budget_mb`, 768; frame-based eviction was 2 minutes
    at 5 FPS). After 1-6 the world shows, green and posterized (the table mapping, fix 5 regions pending a test).
- **CoD4 port on the user's SD (`switch/codmw`, by Lizarddav, binary only, Russian readme):** built on nfsmw-nx's native
  renderer (`nfsmw_nativo_*` strings, offline `.nfsp` shader library), plus CoD4-specific native backends
  (`codmw_backend_*`, GPU skinning, culling) and D3D-level Clear/Resolve handling. Its release `codmw.toml` dvars are
  a ready list for BO1: `r_lodScaleSkinned 1; r_lodScaleRigid 1; r_dof_enable 0; cg_brass 0; fx_marks_smodels 0;
  sm_enable 0; ragdoll_enable 0; r_zfar 0; r_glow 0; snd_khz 22`, GPU 460 MHz, RAM 1600, depth prepass skipped.

## 9. Glossary (short)

ReXGlue: Xbox 360 static recompilation SDK (codegen + Xenia-derived runtime). · XenonRecomp/XenosRecomp:
hedge-dev's CPU and shader recompilers. · NRO: Switch homebrew executable. · libnx/devkitA64: homebrew SDK and
toolchain. · Horizon: Switch OS. · NVK/NAK: Mesa's Vulkan driver and shader compiler for NVIDIA. · PM4 ring: the
Xenos GPU command buffer. · EDRAM: the 360's 10 MB on-die framebuffer memory. · Resolve: copy from EDRAM to a
texture. · Predicated tiling: the 360 renders the frame in tiles to fit EDRAM. · Guard: run native and original
code side by side and compare. · Title takeover / forwarder: ways to launch homebrew with full memory. · TU: Title
Update (`.xexp` delta patch + data files, in an STFS package). · Fastfile (`.ff`): IW engine compressed asset zone.

---

## 10. Reference repositories and links

Cloned in `refs/` (`git clone --depth 1`, no submodules):

| Repo | Notes |
|---|---|
| https://github.com/StevensND/nfsmw-nx | ★ reference; docs/ |
| https://github.com/nebadasSwifty/masseffect-nx | macOS + Docker build, extract_iso.py, 59 docs |
| https://github.com/NaGaa95/skate3recomp-nx | data-driven renderer; SDK submodule `mchughalex/rexglue-skate3` |
| https://github.com/R-drg/the-simpsons-game-nx | VP6 → FFmpeg |
| https://github.com/R-drg/project8-sw | reuse analysis, patch-series SDK, simpler renderer, AGENTS.md |
| https://github.com/liartes/torchlight-nx | CLAUDE.md work log, engine-level renderer |
| https://github.com/NaGaa95/UnleashedRecomp-NX | XenonRecomp; memory barriers patch |
| https://github.com/ChanseyIsTheBest/MarathonRecomp-NX | XenonRecomp; huge perf catalogue |
| https://github.com/LucioXerus/bo1-recomp | ★ BO1 PC recomp (mirror of the deleted ItsManueh/bo1-recomp) |

Other links:
- ReXGlue SDK: https://github.com/rexglue/rexglue-sdk (v0.10.0 is what all the Switch ports pin; docs at
  https://www.mintlify.com/rexglue/rexglue-sdk/quickstart)
- mesa-switch: https://github.com/danfromtico/mesa-switch (nfsmw pins `1a8c1a66d6f`, project8 `d4a00ea0ab3`); NaGaa95
  fork https://github.com/NaGaa95/mesa-switch; NXVK https://github.com/PalindromicBreadLoaf/nxvk
- Installers: https://github.com/StevensND/nfsmw-nx-installer
- Jump tables: https://github.com/xdzleo/xenon-jumptables
- BO2 recomp: https://github.com/officialmelon/bo2-recompiled
- Other Switch ReXGlue ports found (not studied): `ChanseyIsTheBest/sa2-nx`, `ChanseyIsTheBest/futari-nx`,
  `barbaruiva25/fc3-nx` (Far Cry 3), `CypherNoodle/FNV-nx` (Fallout NV), `penettrator/Mercenaries2-NX`,
  `andrewnakas/skate3-switch`, `XDanfr/RaymanOriginsRecomp` (XenonRecomp → Switch)
- Xenia game patches (per-title knobs): https://github.com/xenia-canary/game-patches
- Xenia compatibility issue for BO1 (hangs after "Press Start" in old builds, drawing issues):
  https://github.com/xenia-project/game-compatibility/issues/403
- Digital Foundry face-off (360: 1040x608 2×MSAA, 60 fps):
  https://www.thesixthaxis.com/2010/11/10/black-ops-gets-digital-foundry-face-off/
- Bink (FFmpeg has supported Bink 1 since it was reverse-engineered): https://wiki.mewayz.com/wiki/Bink_Video
- ReXGlue interview/background: https://readonlymemo.com/rexglue-xbox-360-recompilation-interview/
