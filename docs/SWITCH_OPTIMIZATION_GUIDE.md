# Optimizing Xbox 360 → Nintendo Switch ports: a field guide

A reusable guide for porting Xbox 360 games to Nintendo Switch homebrew (static recompilation with ReXGlue/XenonRecomp,
a native Vulkan renderer on NVK, devkitA64/libnx). It collects what we learned porting Call of Duty: Black Ops (2026,
this repository), what the other ports measured (nfsmw-nx, masseffect-nx, project8-sw, the CoD4 port and others), and
general Switch hardware facts. Every claim says where it comes from; numbers are from the console at stock clocks
unless marked otherwise.

How to use it: read §1-2 before writing code, §3 when the native renderer draws wrong or slow, §4 for the game side,
§5-6 to measure and test, §7 for what did not work. §8 is the BO1 timeline as a worked example.

---

## 1. The hardware and what it implies

| Part | Fact | Consequence |
|---|---|---|
| CPU | 4× Cortex-A57 at **1020 MHz** (same docked and handheld); a game gets **3 cores** (0-2), core 3 is the system's | ≈100 core-ms per frame at 30 FPS; single-thread speed is the usual limit for recompiled code |
| CPU caches | L1D 32 KB (2-way), L1I 48 KB, **L2 2 MB shared**, DRAM ~250 cycles | big working sets thrash; code locality matters with a 48 KB I-cache |
| CPU TLB | 32-entry L1 DTLB, 1024-entry L2 TLB (4 MB reach with 4 KB pages), slow misses | map guest memory with **2 MB pages** where possible |
| CPU execution | 40-bundle reorder window, 12-entry store queue, 1 FMA/cycle (10-cycle latency), 5-cycle multiplies | dependent chains and scattered loads are slow |
| Memory | 4 GB LPDDR4 shared CPU/GPU, 25.6 GB/s theoretical, **~8 GB/s from one CPU core**; 1331.2 MHz handheld, 1600 docked | every CPU copy (vertex uploads, untiling, memcpy) competes with the GPU |
| GPU | Maxwell GM20B, 2 SMs, 256 lanes; **307.2 MHz handheld default, 460.8 MHz with the official profile, 768 docked** | handheld GPU is ~2.5× weaker than docked; request the 460.8 profile |
| GPU caches | L1 12 KB per SM pair, **L2 256 KB**, DRAM > 400 ns | bandwidth-heavy passes (full-screen post, big targets, MSAA) are expensive |
| GPU features | packed FP16, ZCULL + early-Z, **no stencil export from shaders** | keep depth writes fixed-function; stencil moves through the copy engine |
| Storage | microSD over UHS-I, ~60-95 MB/s sequential; A2 cards load 15-25 % faster in streaming games | small random reads and rereads hurt; card quality matters |
| Process memory | ~3189 MB with title takeover or a forwarder; ~400 MB in album applet mode | always launch with full memory (hold R on a game, or a forwarder with a 39-bit address space) |

Sources: Chips and Cheese (A57 and GM20B measurements), masseffect-nx/nfsmw-nx `docs/platform-notes.md`, Nintendo
microSD guidance.

---

## 2. Platform rules (Horizon, libnx, NVK)

### Threads and scheduling

- **Only priority 0x3B is time-sliced; every other priority is cooperative.** A busy thread at another priority keeps
  its core, even while other cores are idle. Recompiled games busy-wait constantly, so run guest threads at 0x3B and
  host service threads above them (lower number): audio 0x2B, presentation 0x2C, ring thread 0x2D.
- **When a thread starves, lower the hog; don't raise the starving one** (higher priorities belong to the system).
- **A preferred core is not a pin** (0.24 migrations per loop measured); an exclusive core mask pins. Pinning the game's
  main thread alone on a core helped Mass Effect (+ render thread at 0x2B) but lowered NFS:MW's minimum FPS. A/B it.
- **Replace the game's busy-waits with real waits** (hook the polling function, sleep with a timeout or until woken,
  then run the original so the game's own checks still happen). On the 360 a spinning hardware thread is nearly free;
  here it steals one of three cores.
- **Kernel calls cost microseconds**: signalling a condition variable even with no waiter, fence queries, memory
  mapping, IPC. Avoid per-frame/per-draw syscalls and allocations (ME found a guest timer thread at 92 % of a core from
  `notify_all` per release plus mallocs per wait).
- Never read `cntvct_el0` (faults): use `armGetSystemTick`. `std::thread::detach()` can throw: use persistent workers.
  Decide to sleep or wake only under the lock (lost wake-ups hung NFS:MW's copy thread).
- Synchronous logging to the SD card from hot threads stalls them; use a non-blocking log ring and never log from the
  render/ring thread.

### Memory

- Commit guest memory in **2 MB granules** (kernel memory block limit) and map views 2 MB-congruent so the kernel can use
  block descriptors (ReXGlue's `guest_memory_large_pages`; the CoD4 port enables it).
- Avoid emulated memory accesses (page watching): each fault is expensive, and **Horizon loses vector registers when a
  thread is preempted inside the exception handler** (stale q registers → crashes). This is the main reason every port
  replaces the SDK's emulated Xenos GPU with a native renderer.
- GCC can merge pairs of 64-bit guest-context copies into one q-register load/store; a page fault between them came back
  with q31 cleared (project8-sw). Build the recompiled code with `-fno-tree-slp-vectorize -fno-tree-vectorize`.

### NVK (Mesa) on Horizon

- **Every GPU memory allocation is several kernel round trips** (nvMapCreate, VA reservation, mapping, plus a memset and
  cache clean of the whole allocation). NFS:MW measured ~1.9 ms per dedicated texture allocation; BO1 measured ~8 ms
  per texture (older Mesa, larger textures). **Sub-allocate textures from big slabs** (§3.6).
- The number of NvMap handles is limited (error **0x235C**, "VA bind ... failed" / `nvMapCreate failed`): thousands of
  dedicated allocations run into it after a few minutes.
- Every fence query is an ioctl. The GPFIFO is finite (a full queue looks like a lost device). NVK timestamps read
  ~1.6× low: multiply by 1.627.
- Early-Z and ZCULL are lost if a shader writes depth; one such setting made Mass Effect run under 1 FPS.
- Newer Mesa for Horizon (26.2.3: ZCULL enabled, less shader-cache lock contention, fewer redundant UBO updates) and
  deko3d (an NVN-like API: 8-14 µs per draw vs NVK's 9-21, frames ≥100 ms 4.5 → 1.0 per minute in NFS:MW's branch) are
  the long-term API options.

### Clocks

- Use the official **460.8 MHz handheld GPU profile** (`apm` configuration 0x92220008 = GPU 460.8 + memory 1331.2;
  0x92220007 raises memory to 1600 — pick deliberately). `apmSetPerformanceConfiguration` returns before the clocks
  change; poll before reading.
- **CPU boost mode** (`appletSetCpuBoostMode`, CPU 1785 MHz) throttles the GPU to 76.8 MHz: unusable in play (ME: 3-4
  FPS) but Nintendo's own games use it **during loading screens**.
- Measure at stock clocks. Overclocked numbers hid the real limit for days in NFS:MW.

### Presentation

- One Vulkan queue, three swapchain images, FIFO and IMMEDIATE only (IMMEDIATE doesn't tear; the compositor composes at
  60 Hz). Present from the thread that produces the image. Three work slots and asynchronous output removed a 207 ms
  idle gap in ME.

---

## 3. The native renderer: correctness traps and costs

All Xbox 360 ports of this family keep the game's own Direct3D layer, read its PM4 command ring on a "ring thread" and
record Vulkan. project8-sw's renderer (from nfsmw-nx, GPL-3) is game-independent and a good starting point; it needed
the fixes below for a CoD-engine game. Most are generic: other games will hit them.

### 3.1 EDRAM aliasing: one image per guest format is wrong

The 360 renders into 10 MB of EDRAM; games read the same tiles back in different formats. With one Vulkan image per
(EDRAM base, guest format), a draw in one format and a resolve in another touch different images and the result is
empty (BO1: scene drawn as `k_8_8_8_8_GAMMA`, resolved as `k_8_8_8_8` → black world).
**Fix:** formats that map to the same host VkFormat share an image (canonical key). Mass Effect needed the full
"mode 4" (per-tile ownership + transfers) because UE3 reinterprets HDR words across formats.

### 3.1b A target can start inside another one

BO1 draws its 1024x2048 sun shadow map in two halves: the top at EDRAM base 384, the bottom by moving the base to
1216 (inside the first target), then resolves all 2048 rows from base 384. A renderer with one image per base never
resolves the bottom half. Trace the passes of a frame with shadows on and look for a depth-only pass whose base falls
inside another target of the same pitch: either draw it into that image at a row offset, or (if it was never visible
anyway) skip it.

### 3.2 Clears and resolves through a 4x view at half the pitch

Xbox D3D clears a 2x (or 1x) surface by drawing one rectangle into a **4x MSAA view at half the pitch**, which covers the
same EDRAM tiles (tiles are 80x16 samples; 4x pixels are 2x2 samples, 2x pixels 1x2). Games also **resolve** through
such a view as a free 2x2 downsample (BO1's bloom/exposure chain).
**Fixes:** redirect such clear rectangles to the existing full-pitch target with viewport/scissor scaled; serve a
resolve from a never-drawn 4x view by blitting the 1x image at half size with linear filtering.

### 3.3 Resolve exponent bias

Resolves carry an exponent bias (`RB_COPY_DEST_INFO.copy_dest_exp_bias`). BO1 writes `k_16_16_16_16` targets with RT
bias +5, resolves with +11 and samples with fetch `exp_adjust` −16 (net 0). Ignoring the resolve bias read values 2^11
too small → zero after a few passes → black frame. Host 16_16(_16_16) targets are SNORM at 1/32 scale, so the copy
needs bias + 5 (the SDK's own rule). **Cheap fix:** add the bias to the `exp_adjust` of fetch constants that sample the
destination (the translated shaders apply it at run time) instead of a scaling copy shader.

### 3.4 Textures the renderer may not support

- **3D textures from a 2D resolve**: BO1's 32x32x32 colour-grading LUT is a 32x1024 colour resolve sampled as a tiled
  3D texture. The 2D and 3D tilings address the same bytes but scramble texels in **16x8 blocks**; compute copy regions
  from `GetTiledOffset2D`/`GetTiledOffset3D` (256 regions for that LUT). Stacking slices naively gave a green,
  posterized image.
- **3D textures from memory**: untile with `GetTiledOffset3D` (or linear addressing) plus the fetch's endian swap.
  BO1's 512x256x4 volume was its ambient lighting: without it shadowed areas were black.
- **2D fetches of a 3D texture** exist (BO1 has one): create the 3D image with `VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT`
  and give such fetches a 2D-array view (slices as layers).
- **Depth and colour resolves to the same address**: BO1 resolves shadow depth and then the LUT to one address every
  frame. One destination per address destroyed and recreated two 8 MB images twice a frame (and the LUT's volume never
  survived). Keep a depth and a colour destination per address; sampling takes the kind written last.

### 3.5 Memexport: GPU memory copies

Some games copy memory with the GPU: a memexport vertex shader that fetches from a vertex buffer and writes to a
stream (BO1: point list, 64 bytes per vertex as 8 × `FMT_16_16_16_16` fetches, exported as 8 elements of 8 bytes; its
texture streamer copies **low mips** into place with it). A renderer that skips memexport draws leaves stale data →
rainbow noise on distant/angled surfaces. Diagnose by decoding raw guest bytes offline (level 0 fine, mips garbage in
guest memory). **Fix:** detect that exact shader (ucode hash), validate the stream constant (`eA`: base dwords with
top bits 01, index/format/count as floats in [2^23, 2^24)) and do the copy on the CPU in ring order. Fetch 8in32 +
`.yxwz` and export 8in16 cancel out: it's a byte copy. Then tell the texture cache which range changed (§3.7).

### 3.6 Texture memory: pool, budget, eviction

- **Sub-allocate textures from 32 MB slabs** (nfsmw-nx `texturas_pool`, 64 KB units, first fit, no dedicated
  allocation info so any PTE kind can bind). BO1: image creation on the ring 15-30 → 5-12 ms per frame; worst pause
  1.4 s → ~0.4 s. Grow slabs ahead of need (headroom) but not mid-burst (a new slab costs a memset of its size);
  BO1 needed 48 MB headroom and a slab every 20 frames instead of nfsmw's 8 MB / 120.
- Release a pooled block only after the GPU finished the frames that used the texture (destroy-after-fence).
- **Budget by memory and evict LRU**; frame-count eviction is not time at 5-10 FPS (600 frames was 2 minutes).
- Free GPU memory on a background thread (each free is a kernel round trip).

### 3.7 Texture change detection under streaming

- Hash guest texture data once per frame per layout, with sampled checks and periodic full hashes (project8-sw's
  `ChangeDetector`). Fine for static data.
- When the renderer itself writes guest memory (§3.5), **invalidate only the textures on the written pages**: index
  texture layouts by 64 KB page. Scanning every layout ever seen per write (tens of thousands × ~40 copies a frame) took
  BO1's ring from ~12 to ~60 µs per draw.
- Never clear the whole lookup memo on an invalidation; flag the affected textures.
- New textures created asynchronously appear 1-2 frames late — a visible flicker at 10-15 FPS. Create small/medium
  textures synchronously within a per-frame byte budget (larger budget when a backlog builds, e.g. right after a load),
  and show a predecessor while a replacement builds when one can be matched.

### 3.8 Vertex data

- **Copy only the vertex range a draw reads.** Copying each vertex fetch constant's whole buffer (up to 16 MB) when its
  hash changes cost BO1 ~330 MB per frame and 35 ms of ring CPU on an M4. Walk each vertex shader once (a
  `ShaderTranslator` subclass) and prove every `vfetch_full` reads the untouched `r0.x` (the translator stores
  `clamp((index + VGT_INDX_OFFSET) & 0xFFFFFF, VGT_MIN_VTX_INDX, VGT_MAX_VTX_INDX)` there); then copy
  `[lo*stride, hi*stride + extent)` and rebase the fetch address. Cache index bounds per index buffer by content hash.
  BO1: ~3 MB per frame, ring 35 → 2.7 ms (Mac).
- Copy vertices at submit time, never later (a worker that kept guest pointers corrupted dynamic UI/movie geometry in ME).

### 3.9 Other renderer facts

- Shader identification: D3D rewrites vertex shaders in place at bind; identify by the loaded microcode.
- Write only the vertex outputs the pixel shader reads (ME: scene 66 → 36.6 ms).
- Pipelines: prewarm known pipelines at boot, keep the driver cache on SD, per-stage NVK cache keys; asynchronous
  pipeline creation causes pop-in.
- Cache the translated SPIR-V too, not just the driver's pipelines: translation of a Xenos shader costs far more
  than `vkCreateShaderModule`. Key each record by shader type, microcode hash and modification, and store the
  texture and sampler bindings with it: the SPIR-V translator fills the shader's binding lists as a side effect of
  the first translation, so a hit that only restores the binary leaves the shader with no bindings. Append records
  with a checksum each and stop reading at the first bad one (write the next record over it); key the file by
  vendor, device and driver version plus a version number bumped when the translator changes.
- With the SPIR-V cached, pipelines can be built before the game asks: save every pipeline key (shader identities
  with modifications, layout counts, geometry shader key, fixed state) and compile the list at boot on the worker,
  behind any pipeline a draw is waiting for. Build the key so it alone is enough to create the pipeline. BO1: 62
  compile-linked slow frames per session → 21 with the SPIR-V cache alone.
- Metal (MoltenVK on a Mac) lacks geometry shaders: rectangle lists and point sprites are skipped there, which also
  drops D3D's clears. Keep the Mac for logic and most rendering bugs, but verify on the console.

---

## 4. The game side

- **Fewer passes through the engine's own settings first.** Dynamic shadows were ~65 % of BO1's draws (sun cascades +
  spot lights: ~3,100 of ~4,800); `sm_enable 0` (baked lightmap shadows stay) plus DOF, distortion, flame effects,
  brass, decals on static models and ragdolls off. Apply settings through the game's command buffer (`Cbuf_AddText`)
  from a hook, and re-apply periodically (scripts set some back).
- **Texture streaming is the main stutter source in streaming engines.** BO1 created ~80 new textures a second
  (moves, high mips, memexport copies, SD reads). `r_stream` (an int 0-7, default 7, a bitmask) set to 0 in gameplay
  removed all of it — but:
  - **a map load waits for the streamer**: with streaming off before a load, the load never finishes;
  - rewriting the setting through the command buffer **does not run during a load** (the main thread is in the
    loader), and rewriting it during a load restarts the streamer;
  - a 3D main menu (~1,500 draws) looks like gameplay to a draw-count heuristic.
  **What works:** write the dvar's value **directly in guest memory** (BO1: pointer at `0x83CF6850`; dvar `+0x18`
  current, `+0x28` latched, `+0x38` reset, type int = 5 at `+0x10`), off in gameplay, back to 7 as soon as a loading
  screen shows (a stuck load resumes immediately). Use the dvar's own value as the state.
- Lighter assets: another platform's assets (e.g. the Wii version) can't be loaded by the 360 executable (different
  formats, materials, packages). Use the game's own detail settings (streaming, LOD bias) or skip top mip levels in the
  renderer instead.
- Read the game's dvar table from the decrypted executable (`find_string_callers`, strings `r_*`, `sm_*`): registration
  calls show type, default and range (BO1's `r_stream` turned out to be an int, not a bool).
- Busy-wait hooks, render thread priority and native replacements of hot game functions (with a guard comparing against
  the original) are the main CPU wins on the guest side after the renderer.
- Codegen: direct calls instead of weak aliases, registers as C++ locals, "diet" lowerings, guest memory without
  `volatile` (+15-20 % in ME), `-ffp-contract=off` for exactness. LTO/PGO helped NFS:MW, not ME.
- Bink: if the video is paced by an audio clock that never advances, playback freezes; add a wall-clock fallback.
- Busy SD card: cache reread ranges (ME's range cache); never block the game/render thread on reads.

---

## 5. Measuring

- **Stock clocks, one change per run behind a setting, A/B back to back** (run-to-run noise ~3 FPS).
- **Report frame-time distribution, not just FPS**: share of frames > 33.3 / 50 / 100 ms, range, worst. Frames are
  shown in 16.67 ms steps: a steady 38 ms feels better than 33/50 alternation. A fixed swap interval (2 = 30 FPS,
  3 = 20 FPS) removes cadence jitter once ~90 % of frames fit it; a separate presentation thread raised NFS:MW's average
  but tripled the spread.
- **Know which thread is the limit.** Useful per-10-second renderer report lines (all in this repo's
  `bo1_native_system.cpp`): packets/draws/copies/swaps; renderer CPU and GPU per frame; draws per frame; texture lookup
  time split into **hashing** and **images created on the ring**; memexport copies; texture memory, pool state; slow
  frames with "ring idle" (the game sent nothing: game-side stall) vs busy time.
- **Displayed FPS from a gameplay video** (the console's 30 FPS capture): count distinct frames with
  `ffmpeg -i clip.mp4 -vf "mpdecimate=hi=200:lo=64:frac=0.1,showinfo" -f null -` and compute gaps from `pts_time`
  (median, 90th percentile, max, count > 100 ms). Static scenes undercount slightly.
- GPU timestamps on NVK read ~1.6× low.
- The SDK stack profiler (`logs/rex/stacks_profile.flag` next to the NRO) answers "which guest function".

---

## 6. Testing and debugging workflow that worked

- **Wi-Fi test loop**: nxlink from the Homebrew Menu/Sphaira NetLoader (hold R on a game for full memory, press Y), the
  NRO gets `--log_network=<mac>:<port>` and streams every log line over TCP to a listener on the Mac
  (`tools/nxlink_run.sh`). Pass settings through `EXTRA_ARGS` (`+` for spaces). The netloader changes `argv[0]`: pass
  `--game_data_root` explicitly.
- **Automatic map load**: queue a console command (`devmap <map>`) through the game's `Cbuf_AddText` from a hook after a
  delay, so tests need no menu navigation.
- **One-frame trace** (`bo1_debug_trace_frame` / `bo1_debug_trace_after_s` — trigger on the first heavy frame after the
  delay, not on time alone, or it lands on a loading screen): every render pass with targets, every resolve with source,
  destination, format, exp bias and clear flags, skipped draws with reason, details of small post-processing draws
  (shaders, RT exp bias, sampled textures with exp_adjust), pixel shader disassembly, and a texture format census.
- **Image dumps**: read back each resolve destination (and chosen textures and mip levels) and save them; on the console
  send them over TCP (`bo1_debug_dump_host` + `tools/dump_receiver.py`, bounced through a heap buffer because Horizon's
  socket service can't read GPU-mapped memory). Convert with `tools/rgbx_to_png.py` (decodes RGBA8, RGB10A2, RGBA16
  F/UNORM/SNORM, R32F depth, R5G6B5, RG8 and BC1-BC5). Seeing the scene image vs the final image located every
  black-screen cause.
- **Raw guest memory dumps + offline decoding** (port `GetTiledOffset2D/3D` to Python, apply the endian swap, decode BC)
  tell renderer bugs from data bugs.
- **Entropy check** of texture data: real DXT ≈ 6 bits/byte; ~7+ looks compressed/garbage.
- **Mac build (MoltenVK) as a fast test bench** for logic, crashes and most renderer bugs; it cannot show draws that
  need geometry shaders (and so misses D3D's clears) and hides CPU costs that the A57 exposes.
- **SD cards fail silently**: blocks keep old contents after writes, and the console can misread files the PC reads
  fine. Verify after remount, hash files on the console (FNV per MiB) against the ISO, never overwrite in place
  (quarantine bad files so their clusters stay allocated), replace failing cards.

---

## 7. What did not work (here or in the other ports)

- Turning a game system off with a dvar can leave its shaders reading stale data: BO1 with `sm_enable 0` lit models
  from shadow data that was no longer drawn, and they alternated between lit and dark every frame near lights. Keep
  the system on and shrink what it draws instead (fewer lights, fewer casters, shorter range). Check each dvar with a
  frame trace: some are registered but unused (`sm_sunEnable`, `sm_spotEnable` in BO1).

| Idea | Result |
|---|---|
| The SDK's emulated Xenos GPU on Switch | 1-3 FPS and crashes (page watching faults + lost vector registers) |
| Overclocking as the target | hides the real limit |
| Lower resolution while CPU-bound; resolutions off the 16-px grid | no gain or worse |
| A separate presentation thread | higher average, 3× wider frame-time spread |
| Batching barriers/transfers | no change (volume is the cost) |
| Arguments in registers on top of register locals | no gain (ME) |
| LTO in ME | no gain (it did help NFS:MW together with PGO) |
| Asynchronous pipeline compile | pop-in |
| Official CPU boost in play | GPU throttled to 76.8 MHz |
| Clearing the whole texture lookup memo on every invalidation (BO1) | ring 12 → 60 µs per draw |
| Creating more textures synchronously without a memory pool (BO1) | 8 ms per texture on the ring |
| Turning texture streaming off before a load / through the command buffer (BO1) | the load froze |
| Predecessor stand-ins keyed by the mips address (BO1) | never matched: the streamer moves the mips too |

---

## 8. Worked example: BO1 Kino der Toten, step by step (2026-10-13 → 10-15)

| Step | Result |
|---|---|
| Emulated GPU | boot, videos, menus at ~1 FPS; Kino loaded only after the SD card's bad blocks were found |
| Native renderer (project8-sw) | menu 11-12 FPS; Kino 4-6 FPS, 3D world black |
| Ranged vertex copies | ~330 → ~3 MB/frame of vertex copies |
| EDRAM format sharing, 4x clear redirect, 2x2 resolve downsample, resolve exp bias, 3D LUT with tiled regions, depth/colour destinations per address | world visible with correct colour grading |
| Memexport GPU memcpy on the CPU | rainbow noise on distant surfaces gone |
| 3D texture from memory (ambient volume) | shadows no longer black |
| Dynamic shadows and minor effects off (`bo1_dvars`) | ~4,800 → ~1,550 draws per frame; ~11-14 FPS |
| Page-indexed invalidation, texture pool | ring CPU ~75 → ~40 ms per frame; worst pause 1.4 s → 0.4 s |
| Texture streaming off in gameplay (`r_stream` written directly, restored at loading screens) | renderer ~20-27 ms, near-zero frames over 100 ms in the first minute; 20 FPS in video |
| Next | a 20 FPS cap for a steady cadence; depth prepass skip and 960x544 toward 30 FPS; game-side stalls with the profiler |

---

## Sources

- masseffect-nx `docs/` (platform-notes, performance-history, optimization-paths, measuring, streaming-io,
  kernel-waits, cold-start-hitches, native-renderer, zero-copy-vertices); nfsmw-nx `docs/` (platform-notes,
  performance-history, measuring, native-renderer) and `app/src/nfsmw_nativo_texturas_pool.*`; project8-sw
  `docs/native-renderer.md`; the CoD4 port's release `codmw.toml`.
- Chips and Cheese: "Cortex A57, Nintendo Switch's CPU" and "Nintendo Switch's iGPU: Maxwell Nerfed Edition".
- GDC 2019 "Witcher 3 on the Nintendo Switch: CPU & Memory Optimization" (slides behind GDC Vault).
- nfsmw-nx PR #27 (deko3d renderer); mesa-switch PR #4 (Mesa 26.2.3 for Horizon); Phoronix on NVK ZCULL.
- Digital Foundry / Nintendo Life on boost mode in loading screens; Engadget on Wolfenstein II's dynamic resolution.
- sa2-nx `docs/porting-guide.md`, `enhancements.md`, `gpu-optimisation-candidates.md`; futari-nx `xbox360.md` (five
  ports' shared notebook); UnleashedRecomp-NX `docs/SWITCH-PERFORMANCE.md`; the-simpsons-game-nx Mesa patches.
  Summarised and ranked for this port in `docs/OTHER_PORTS_RESEARCH.md`.
- This repository: `PORTING_RESEARCH.md` (§8b-8d), `OPTIMIZATION_PLAN.md`, `app/src/native/`, `app/src/bo1_engine.cpp`.
